#include "EigenVisIter.hpp"

#include "chimeMetadata.h"
#include "errors.h"
#include "fpga_header_functions.h"
#include "prometheusMetrics.hpp"
#include "visBuffer.hpp"
#include "LinearAlgebra.hpp"
#include "visUtil.hpp"

#include "fmt.hpp"

#include <cblas.h>
#include <blaze/Blaze.h>

using kotekan::bufferContainer;
using kotekan::Config;
using kotekan::prometheusMetrics;
using kotekan::Stage;


REGISTER_KOTEKAN_STAGE(EigenVisIter);

EigenVisIter::EigenVisIter(Config& config, const string& unique_name, bufferContainer& buffer_container) :
    Stage(config, unique_name, buffer_container, std::bind(&EigenVisIter::main_thread, this)) {

    in_buf = get_buffer("in_buf");
    register_consumer(in_buf, unique_name.c_str());
    out_buf = get_buffer("out_buf");
    register_producer(out_buf, unique_name.c_str());
    _num_eigenvectors = config.get<uint32_t>(unique_name, "num_ev");
    _num_diagonals_filled = config.get_default<uint32_t>(unique_name, "num_diagonals_filled", 0);
    // Read a list from the config, but permit it to be absent (implying empty).
    try {
        for (uint32_t e : config.get<std::vector<uint32_t>>(unique_name, "exclude_inputs")) {
            _exclude_inputs.push_back(e);
        }
    } catch (std::runtime_error const& ex) {
        // Missing, leave empty.
    }
    _num_ev_conv = config.get<uint32_t>(unique_name, "num_ev_conv");
    _tol_eval = config.get_default<double>(unique_name, "tol_eval", 1e-6);
    _tol_evec = config.get_default<double>(unique_name, "tol_evec", 1e-5);
    _max_iterations = config.get_default<uint32_t>(unique_name, "max_iterations", 15);

    // Create the state describing the eigenvalues
    auto& dm = datasetManager::instance();
    // TODO: add a state parameter describing the method used
    state_uptr ev_state = std::make_unique<eigenvalueState>(_num_eigenvectors);
    ev_state_id = dm.add_state(std::move(ev_state)).first;
}

dset_id_t EigenVisIter::change_dataset_state(dset_id_t input_dset_id) const {
    auto& dm = datasetManager::instance();
    return dm.add_dataset(input_dset_id, ev_state_id);
}

void EigenVisIter::main_thread() {

    frameID input_frame_id(in_buf);
    frameID output_frame_id(out_buf);

    dset_id_t _output_dset_id = 0;

    DynamicHermitian<float> mask;
    uint32_t num_elements = 0;
    bool initialized = false;

    openblas_set_num_threads(1);

    while (!stop_thread) {

        // Containers for results
        eig_t<cfloat> eigpair;
        EigConvergenceStats stats;

        // Get input visibilities. We assume the shape of these doesn't change.
        if (wait_for_full_frame(in_buf, unique_name.c_str(), input_frame_id) == nullptr) {
            break;
        }
        auto input_frame = visFrameView(in_buf, input_frame_id);

        // check if the input dataset has changed
        if (input_dset_id != input_frame.dataset_id) {
            input_dset_id = input_frame.dataset_id;
            _output_dset_id = change_dataset_state(input_dset_id);
        }

        // Check that we have the full triangle
        uint32_t num_prod_full = input_frame.num_elements *
            (input_frame.num_elements + 1) / 2;
        if (input_frame.num_prod != num_prod_full) {
            throw std::runtime_error("Eigenvectors require full correlation"
                                     " triangle");
        }

        // Start the calculation clock.
        double start_time = current_time();

        // Initialise the mask
        if (!initialized) {
            num_elements = input_frame.num_elements;
            mask = calculate_mask(num_elements);
            initialized = true;
        }

        // Copy the visibilties into a blaze container
        DynamicHermitian<cfloat> vis = to_blaze_herm(input_frame.vis);

        // Perform the actual eigen-decomposition
        std::tie(eigpair, stats) = eigen_masked_subspace(vis, mask, _num_eigenvectors, _tol_eval,
                                                         _tol_evec, _max_iterations, _num_ev_conv);
        auto& evals = eigpair.first;
        auto& evecs = eigpair.second;

        // Stop the calculation clock. This doesn't include time to copy stuff into
        // the buffers, but that has to wait for one to be available.
        double elapsed_time = current_time() - start_time;

        // Report all eigenvalues to stdout.
        std::string str_evals = "";
        for (uint32_t i = 0; i < _num_eigenvectors; i++) {
            str_evals += " " + std::to_string(evals[i]);
        }
        DEBUG("Found eigenvalues:%s, with RMS residuals: %e, in %3.1f s. Took %i/%i iterations.",
              str_evals.c_str(), stats.rms, elapsed_time, stats.iterations, _max_iterations);

        // Update Prometheus metrics
        update_metrics(input_frame.freq_id, input_frame.dataset_id, elapsed_time, eigpair, stats);

        /* Write out new frame */
        // Get output buffer for visibilities. Essentially identical to input buffers.
        if (wait_for_empty_frame(out_buf, unique_name.c_str(), output_frame_id) == nullptr) {
            break;
        }
        allocate_new_metadata_object(out_buf, output_frame_id);
        auto output_frame = visFrameView(out_buf, output_frame_id, input_frame.num_elements,
                                         input_frame.num_prod, _num_eigenvectors);

        // Copy over metadata and data, but skip all ev members which may not be
        // defined
        output_frame.copy_metadata(input_frame);
        output_frame.dataset_id = _output_dset_id;
        output_frame.copy_data(input_frame, {visField::eval, visField::evec, visField::erms});

        // Copy in eigenvectors and eigenvalues.
        for (uint32_t i = 0; i < _num_eigenvectors; i++) {
            int indr = _num_eigenvectors - 1 - i;
            output_frame.eval[i] = evals[indr];

            for (uint32_t j = 0; j < num_elements; j++) {
                output_frame.evec[i * num_elements + j] = evecs(j, indr);
            }
        }
        // HACK: return the convergence state in the RMS field (negative == not
        // converged)
        output_frame.erms = stats.converged ? stats.rms : -stats.eps_eval;

        // Finish up interation.
        mark_frame_empty(in_buf, unique_name.c_str(), input_frame_id++);
        mark_frame_full(out_buf, unique_name.c_str(), output_frame_id++);
    }
}


// TODO: this should probably be a private method, but that would mean exposing all the blaze stuff and [j]
void EigenVisIter::update_metrics(uint32_t freq_id, dset_id_t dset_id, double elapsed_time,
                                  const eig_t<cfloat>& eigpair,
                                  const EigConvergenceStats& stats)
{
    // Update average write time in prometheus
    auto key = std::make_pair(freq_id, dset_id);
    auto& calc_time = calc_time_map[key];
    calc_time.add_sample(elapsed_time);
    prometheusMetrics::instance().add_stage_metric("kotekan_eigenvisiter_comp_time_seconds",
                                                   unique_name, calc_time.average());

    // Output eigenvalues to prometheus
    for (uint32_t i = 0; i < _num_eigenvectors; i++) {
        std::string labels = fmt::format("eigenvalue=\"{}\",freq_id=\"{}\",dataset_id=\"{}\"",
                                         i, freq_id, dset_id);
        prometheusMetrics::instance().add_stage_metric("kotekan_eigenvisiter_eigenvalue",
                                                       unique_name,
                                                       eigpair.first[_num_eigenvectors - 1 - i],
                                                       labels);
    }

    // Output RMS to prometheus
    std::string labels = fmt::format("eigenvalue=\"rms\",freq_id=\"{}\",dataset_id=\"{}\"",
                                     freq_id, dset_id);
    prometheusMetrics::instance().add_stage_metric("kotekan_eigenvisiter_eigenvalue", unique_name,
                                                   stats.rms, labels);

    // Output convergence stats
    labels = fmt::format("freq_id=\"{}\",dataset_id=\"{}\"", freq_id, dset_id);
    prometheusMetrics::instance().add_stage_metric("kotekan_eigenvisiter_iterations", unique_name,
                                                   stats.iterations, labels);
    prometheusMetrics::instance().add_stage_metric("kotekan_eigenvisiter_eigenvalue_convergence",
                                                   unique_name, stats.eps_eval, labels);
    prometheusMetrics::instance().add_stage_metric("kotekan_eigenvisiter_eigenvector_convergence",
                                                   unique_name, stats.eps_evec, labels);


}


DynamicHermitian<float> EigenVisIter::calculate_mask(uint32_t num_elements) const
{
    blaze::DynamicMatrix<float, blaze::columnMajor> M;
    M.resize(num_elements, num_elements);

    // Construct the mask matrix ...
    // Go through and zero out data in excluded rows and columns.
    M = 1.0f;
    for (auto iexclude : _exclude_inputs) {
        for (uint32_t j = 0; j < num_elements; j++) {
            M(iexclude, j) = 0.0;
            M(j, iexclude) = 0.0;
        }
    }
    // Remove close elements
    int nd = (int)_num_diagonals_filled;
    for (int i = (1 - nd); i < nd; i++) {
        blaze::band(M, i) = 0.0;
    }

    return blaze::declherm(M);

}
