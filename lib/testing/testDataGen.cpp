#include "Config.hpp"       // for Config
#include "StageFactory.hpp" // for REGISTER_KOTEKAN_STAGE, StageMakerTemplate
#include "buffer.h"         // for Buffer, allocate_new_metadata_object, mark_frame_full
#include "chimeMetadata.h"  // for set_first_packet_recv_time, set_fpga_seq_num, set_stream_id
#include "errors.h"         // for exit_kotekan, CLEAN_EXIT, ReturnCode
#include "hfbMetadata.hpp"  // for set_fpga_seq_num, set_dataset_id, set_num_beams

#include <assert.h>    // for assert
#include <atomic>      // for atomic_bool
#include <cmath>       // for fmod
#include <exception>   // for exception
#include <functional>  // for _Bind_helper<>::type, _Placeholder, bind, _1, _2, function
#include <regex>       // for match_results<>::_Base_type
#include <stdexcept>   // for runtime_error
#include <stdint.h>    // for uint8_t, uint64_t
#include <stdlib.h>    // for rand, srand
#include <sys/time.h>  // for gettimeofday, timeval
#include <sys/types.h> // for uint
#include <unistd.h>    // for usleep
#include <vector>      // for vector
// Needed for a bunch of time utilities.
#include "bufferContainer.hpp" // for bufferContainer
#include "dataset.hpp"         // for dset_id_t
#include "datasetManager.hpp"  // for datasetManager
#include "gpsTime.h"           // for FPGA_PERIOD_NS
#include "kotekanLogging.hpp"  // for DEBUG, INFO
#include "restServer.hpp"      // for restServer, connectionInstance, HTTP_RESPONSE, HTTP_RESPO...
#include "testDataGen.hpp"
#include "version.h"   // for get_git_commit_hash
#include "visUtil.hpp" // for current_time


using kotekan::bufferContainer;
using kotekan::Config;
using kotekan::Stage;

using kotekan::connectionInstance;
using kotekan::HTTP_RESPONSE;
using kotekan::restServer;

REGISTER_KOTEKAN_STAGE(testDataGen);


testDataGen::testDataGen(Config& config, const std::string& unique_name,
                         bufferContainer& buffer_container) :
    Stage(config, unique_name, buffer_container, std::bind(&testDataGen::main_thread, this)) {

    buf = get_buffer("out_buf");
    register_producer(buf, unique_name.c_str());
    type = config.get<std::string>(unique_name, "type");
    assert(type == "const" || type == "random" || type == "ramp" || type == "tpluse");
    if (type == "const" || type == "random" || type == "ramp")
        value = config.get<int>(unique_name, "value");
    _pathfinder_test_mode = config.get_default<bool>(unique_name, "pathfinder_test_mode", false);

    samples_per_data_set = config.get_default<int>(unique_name, "samples_per_data_set", 32768);
    stream_id = config.get_default<int>(unique_name, "stream_id", 0);
    num_frames = config.get_default<int>(unique_name, "num_frames", -1);
    // Try to generate data based on `samples_per_dataset` cadence or else just generate it as
    // fast as possible.
    wait = config.get_default<bool>(unique_name, "wait", true);
    // Whether to wait for is rest signal to start or generate next frame. Useful for testing
    // stages that must interact rest commands. Valid modes are "start", "step", and "none".
    rest_mode = config.get_default<std::string>(unique_name, "rest_mode", "none");
    assert(rest_mode == "none" || rest_mode == "start" || rest_mode == "step");
    step_to_frame = 0;
    _first_frame_index = config.get_default<uint32_t>(unique_name, "first_frame_index", 0);
    _gen_all_const_data = config.get_default<bool>(unique_name, "gen_all_const_data", false);

    if (config.exists(unique_name, "dataset_id")) {
        _dset_id = config.get<dset_id_t>(unique_name, "dataset_id");
        _fixed_dset_id = true;
    } else
        _fixed_dset_id = false;

    _num_elements = config.get_default<size_t>(unique_name, "num_elements", 4);
    _num_eigenvectors = config.get_default<size_t>(unique_name, "num_ev", 0);
    // Get the frequency IDs that are on this stream, check the config or just
    // assume all CHIME channels
    // TODO: CHIME specific
    if (config.exists(unique_name, "freq_ids")) {
        freq_ids = config.get<std::vector<uint32_t>>(unique_name, "freq_ids");
    } else {
        freq_ids.resize(1024);
        std::iota(std::begin(freq_ids), std::end(freq_ids), 0);
    }
    _num_beams = config.get_default<uint32_t>(unique_name, "num_frb_total_beams", 1024);
    _factor_upchan = config.get_default<uint32_t>(unique_name, "factor_upchan", 128);
    _init_dataset_manager = config.get_default<bool>(unique_name, "init_dm", false);

    endpoint = unique_name + "/generate_test_data";
    using namespace std::placeholders;
    restServer::instance().register_post_callback(
        endpoint, std::bind(&testDataGen::rest_callback, this, _1, _2));
}


testDataGen::~testDataGen() {
    restServer::instance().remove_json_callback(endpoint);
}

bool testDataGen::can_i_go(int frame_id_abs) {
    if (rest_mode == "none")
        return true;
    if (step_to_frame > 0 && rest_mode == "start")
        return true;
    // Yes, this is a race condition, but it is fine since don't need perfect synchorization.
    if (frame_id_abs < step_to_frame)
        return true;
    return false;
}


void testDataGen::rest_callback(connectionInstance& conn, nlohmann::json& request) {
    int num_frames;
    try {
        num_frames = request["num_frames"];
    } catch (...) {
        conn.send_error("Could not parse number of frames.", HTTP_RESPONSE::BAD_REQUEST);
        return;
    }
    conn.send_empty_reply(HTTP_RESPONSE::OK);
    step_to_frame += num_frames;
}


void testDataGen::main_thread() {

    int frame_id = 0;
    int frame_id_abs = 0;
    uint8_t* frame = nullptr;
    uint64_t seq_num = samples_per_data_set * _first_frame_index;
    bool finished_seeding_consant = false;
    static struct timeval now;

    int link_id = 0;

    dset_id_t ds_id = dset_id_t::null;
    if (_init_dataset_manager) {
        auto& dm = datasetManager::instance();

        // Generate dataset_id
        if (_fixed_dset_id) {
            ds_id = _dset_id;
        } else {
            std::vector<state_id_t> states;
            states.push_back(
                dm.create_state<metadataState>("not set", "FakeVis", get_git_commit_hash()).first);

            std::vector<std::pair<uint32_t, freq_ctype>> fspec;
            // TODO: CHIME specific
            std::transform(std::begin(freq_ids), std::end(freq_ids), std::back_inserter(fspec),
                           [](const uint32_t& id) -> std::pair<uint32_t, freq_ctype> {
                               return {id, {800.0 - 400.0 / 1024 * id, 400.0 / 1024}};
                           });
            states.push_back(dm.create_state<freqState>(fspec).first);

            std::vector<input_ctype> ispec;
            for (uint32_t i = 0; i < _num_elements; i++)
                ispec.emplace_back((uint32_t)i, fmt::format(fmt("dm_input_{:d}"), i));
            states.push_back(dm.create_state<inputState>(ispec).first);

            std::vector<prod_ctype> pspec;
            for (uint16_t i = 0; i < _num_elements; i++)
                for (uint16_t j = i; j < _num_elements; j++)
                    pspec.push_back({i, j});
            states.push_back(dm.create_state<prodState>(pspec).first);
            states.push_back(dm.create_state<eigenvalueState>(_num_eigenvectors).first);

            // Create the beam indices
            states.push_back(dm.create_state<beamState>(_num_beams).first);

            // Create the sub-frequencies specification
            states.push_back(dm.create_state<subfreqState>(_factor_upchan).first);
    
            const std::string weight_type = "hfb_weight_type";
            const std::string git_tag = get_git_commit_hash();
            const std::string instrument_name =
              config.get_default<std::string>(unique_name, "instrument_name", "chime");
            states.push_back(
                dm.create_state<metadataState>(weight_type, instrument_name, git_tag).first);

            // Register a root state
            ds_id = dm.add_dataset(states);
        }
    }

    while (!stop_thread) {
        double start_time = current_time();

        if (!can_i_go(frame_id_abs)) {
            usleep(1e5);
            continue;
        }

        frame = (uint8_t*)wait_for_empty_frame(buf, unique_name.c_str(), frame_id);
        if (frame == nullptr)
            break;

        allocate_new_metadata_object(buf, frame_id);
        set_fpga_seq_num(buf, frame_id, seq_num);

        // Set metadata based on buffer type
        if (!strcmp(buf->buffer_type, "vis")) {
            set_stream_id(buf, frame_id, stream_id);
        } else if (!strcmp(buf->buffer_type, "hfb")) {
            set_dataset_id(buf, frame_id, ds_id);
            set_num_beams(buf, frame_id, _num_beams);
        }

        gettimeofday(&now, nullptr);
        set_first_packet_recv_time(buf, frame_id, now);

        // std::random_device rd;
        // std::mt19937 gen(rd());
        // std::uniform_int_distribution<> dis(0, 255);
        if (type == "random")
            srand(value);
        unsigned char temp_output;
        int num_elements = buf->frame_size / sizeof(uint8_t) / samples_per_data_set;
        for (uint j = 0; j < buf->frame_size / sizeof(uint8_t); ++j) {
            if (type == "const") {
                if (finished_seeding_consant)
                    break;
                frame[j] = value;
            } else if (type == "ramp") {
                frame[j] = fmod(j * value, 256 * value);
                //                frame[j] = j*value;
            } else if (type == "random") {
                unsigned char new_real;
                unsigned char new_imaginary;
                new_real = rand() % 16;
                new_imaginary = rand() % 16;
                temp_output = ((new_real << 4) & 0xF0) + (new_imaginary & 0x0F);
                frame[j] = temp_output;
            } else if (type == "tpluse") {
                frame[j] = seq_num + j / num_elements + j % num_elements;
            }
        }
        DEBUG("Generated a {:s} test data set in {:s}[{:d}]", type, buf->buffer_name, frame_id);

        mark_frame_full(buf, unique_name.c_str(), frame_id);

        frame_id_abs += 1;
        if (num_frames >= 0 && frame_id_abs >= num_frames) {
            INFO("Frame ID greater than the no. of frames");
            exit_kotekan(ReturnCode::CLEAN_EXIT);
            break;
        };
        frame_id = frame_id_abs % buf->num_frames;

        if (_pathfinder_test_mode == true) {
            // Test PF seq_num increment.
            if (link_id == 7) {
                link_id = 0;
                seq_num += samples_per_data_set;
            } else {
                link_id++;
            }
        } else {
            seq_num += samples_per_data_set;
        }
        if (frame_id == 0 && !_gen_all_const_data)
            finished_seeding_consant = true;

        if (wait) {
            double time = current_time();
            double frame_end_time =
                (start_time + (float)samples_per_data_set * FPGA_PERIOD_NS * 1e-9);
            if (time < frame_end_time)
                usleep((int)(1e6 * (frame_end_time - time)));
        }
    }
}
