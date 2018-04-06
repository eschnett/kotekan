
#include "visFileH5.hpp"
#include "errors.h"
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <libgen.h>

#include <highfive/H5DataSet.hpp>
#include <highfive/H5DataSpace.hpp>
#include <highfive/H5File.hpp>

using namespace HighFive;


// Register the HDF5 file writers
REGISTER_VIS_FILE("hdf5", visFileH5);
REGISTER_VIS_FILE("hdf5fast", visFileH5Fast);


#if defined( __APPLE__ )
// Taken from
// https://android.googlesource.com/platform/system/core/+/master/base/include/android-base/macros.h
#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(exp)            \
  ({                                       \
    decltype(exp) _rc;                     \
    do {                                   \
      _rc = (exp);                         \
    } while (_rc == -1 && errno == EINTR); \
    _rc;                                   \
  })
#endif
#endif


std::string create_lockfile(std::string filename) {

    // Create the lock file first such that there is no time the file is
    // unlocked
    std::string dir = filename;
    std::string base = filename;
    dir = dirname(&dir[0]);
    base = basename(&base[0]);

    std::string lock_filename = dir + "/." + base + ".lock";
    std::ofstream lock_file(lock_filename);
    lock_file << getpid() << std::endl;
    lock_file.close();

    return lock_filename;
}

//
// Implementation of standard HDF5 visibility data file
//

void visFileH5::create_file(const std::string& name,
                            const std::map<std::string, std::string>& metadata,
                            const std::vector<freq_ctype>& freqs,
                            const std::vector<input_ctype>& inputs,
                            const std::vector<prod_ctype>& prods,
                            size_t num_ev, size_t num_time) {

    std::string data_filename = name + ".h5";

    lock_filename = create_lockfile(data_filename);

    // Determine whether to write the eigensector or not...
    write_ev = (num_ev > 0);

    INFO("Creating new output file %s", name.c_str());

    file = std::unique_ptr<File>(
        new File(data_filename, File::ReadWrite | File::Create | File::Truncate)
    );
    create_axes(freqs, inputs, prods, num_ev, num_time);
    create_datasets();

    // Write out metadata into flle
    for (auto item : metadata) {
        file->createAttribute<std::string>(
            item.first, DataSpace::From(item.second)
        ).write(item.second);
    }

    // Add weight type flag where gossec expects it    
    dset("vis_weight").createAttribute<std::string>(
        "type", DataSpace::From(metadata.at("weight_type"))
    ).write(metadata.at("weight_type"));
}

visFileH5::~visFileH5() {
    file->flush();
    file.reset(nullptr);
    std::remove(lock_filename.c_str());
}

void visFileH5::create_axes(const std::vector<freq_ctype>& freqs,
                            const std::vector<input_ctype>& inputs,
                            const std::vector<prod_ctype>& prods,
                            size_t num_ev, size_t num_time = 0) {

    create_time_axis(num_time);

    // Create and fill other axes
    create_axis("freq", freqs);
    create_axis("input", inputs);
    create_axis("prod", prods);

    if(write_ev) {
        std::vector<uint32_t> ev_vector(num_ev);
        std::iota(ev_vector.begin(), ev_vector.end(), 0);
        create_axis("ev", ev_vector);
    }
}

template<typename T>
void visFileH5::create_axis(std::string name, const std::vector<T>& axis) {

    Group indexmap = file->exist("index_map") ?
                     file->getGroup("index_map") :
                     file->createGroup("index_map");
    
    DataSet index = indexmap.createDataSet<T>(name, DataSpace(axis.size()));
    index.write(axis);
}

void visFileH5::create_time_axis(size_t num_time) {

    Group indexmap = file->exist("index_map") ?
                     file->getGroup("index_map") :
                     file->createGroup("index_map");
    
    DataSet time_axis = indexmap.createDataSet(
      "time", DataSpace({0}, {num_time}),
      create_datatype<time_ctype>(), std::vector<size_t>({1})
    );
}


void visFileH5::create_datasets() {

    Group flags = file->createGroup("flags");

    create_dataset("vis", {"time", "freq", "prod"}, create_datatype<cfloat>());
    create_dataset("flags/vis_weight", {"time", "freq", "prod"}, create_datatype<float>());
    create_dataset("gain_coeff", {"time", "freq", "input"}, create_datatype<cfloat>());
    create_dataset("gain_exp", {"time", "input"}, create_datatype<int>());

    // Only write the eigenvector datasets if there's going to be anything in
    // them
    if(write_ev) {
        create_dataset("eval", {"time", "freq", "ev"}, create_datatype<float>());
        create_dataset("evec", {"time", "freq", "ev", "input"}, create_datatype<cfloat>());
        create_dataset("erms", {"time", "freq"}, create_datatype<float>()); 
    }

    file->flush();

}

void visFileH5::create_dataset(const std::string& name, const std::vector<std::string>& axes,
                               DataType type) {

    size_t max_time = dset("index_map/time").getSpace().getMaxDimensions()[0];

    // Mapping of axis names to sizes (start, max, chunk)
    std::map<std::string, std::tuple<size_t, size_t, size_t>> size_map;
    size_map["freq"] = std::make_tuple(length("freq"), length("freq"), 1);
    size_map["input"] = std::make_tuple(length("input"), length("input"), length("input"));
    size_map["prod"] = std::make_tuple(length("prod"), length("prod"), length("prod"));
    size_map["ev"] = std::make_tuple(length("ev"), length("ev"), length("ev"));
    size_map["time"] = std::make_tuple(0, max_time, 1);

    std::vector<size_t> cur_dims, max_dims, chunk_dims;

    for(auto axis : axes) {
        auto cs = size_map[axis];
        cur_dims.push_back(std::get<0>(cs));
        max_dims.push_back(std::get<1>(cs));
        chunk_dims.push_back(std::get<2>(cs));
    }
    
    DataSpace space = DataSpace(cur_dims, max_dims);
    DataSet dset = file->createDataSet(
        name, space, type, max_dims
    );
    dset.createAttribute<std::string>(
        "axis", DataSpace::From(axes)).write(axes);
}

// Quick functions for fetching datasets and dimensions
DataSet visFileH5::dset(const std::string& name) {
    const std::string dset_name = name == "vis_weight" ? "flags/vis_weight" : name;
    return file->getDataSet(dset_name);
}

size_t visFileH5::length(const std::string& axis_name) {
    if(!write_ev && axis_name == "ev") return 0;
    return dset("index_map/" + axis_name).getSpace().getDimensions()[0];
}

size_t visFileH5::num_time() {
    return length("time");
}


uint32_t visFileH5::extend_time(time_ctype new_time) {

    // Get the current dimensions
    size_t ntime = length("time"), nprod = length("prod"),
           ninput = length("input"), nfreq = length("freq"),
           nev = length("ev");

    INFO("Current size: %zd; new size: %zd", ntime, ntime + 1);
    // Add a new entry to the time axis
    ntime++;
    dset("index_map/time").resize({ntime});
    dset("index_map/time").select({ntime - 1}, {1}).write(&new_time);

    // Extend all other datasets
    dset("vis").resize({ntime, nfreq, nprod});
    dset("vis_weight").resize({ntime, nfreq, nprod});
    dset("gain_coeff").resize({ntime, nfreq, ninput});
    dset("gain_exp").resize({ntime, ninput});

    if(write_ev) {
        dset("eval").resize({ntime, nfreq, nev});
        dset("evec").resize({ntime, nfreq, nev, ninput});
        dset("erms").resize({ntime, nfreq});
    }

    // Flush the changes
    file->flush();

    return ntime - 1;
}


void visFileH5::write_sample(
    uint32_t time_ind, uint32_t freq_ind, std::vector<cfloat> new_vis,
    std::vector<float> new_weight, std::vector<cfloat> new_gcoeff,
    std::vector<int32_t> new_gexp, std::vector<float> new_eval,
    std::vector<cfloat> new_evec, float new_erms
) {

    // Get the current dimensions
    size_t nprod = length("prod"), ninput = length("input"), nev = length("ev");

    dset("vis").select({time_ind, freq_ind, 0}, {1, 1, nprod}).write(new_vis);
    dset("vis_weight").select({time_ind, freq_ind, 0}, {1, 1, nprod}).write(new_weight);
    dset("gain_coeff").select({time_ind, freq_ind, 0}, {1, 1, ninput}).write(new_gcoeff);
    dset("gain_exp").select({time_ind, 0}, {1, ninput}).write(new_gexp);

    if(write_ev) {
        dset("eval").select({time_ind, freq_ind, 0}, {1, 1, nev}).write(new_eval);
        dset("evec").select({time_ind, freq_ind, 0, 0}, {1, 1, nev, ninput}).write((const cfloat *)new_evec.data());
        dset("erms").select({time_ind, freq_ind}, {1, 1}).write(new_erms);
    }

    file->flush();
}


//
// Implementation of the fast HDF5 visibility data file
//

// Implement the create_file method
void visFileH5Fast::create_file(
    const std::string& name,
    const std::map<std::string, std::string>& metadata,
    const std::vector<freq_ctype>& freqs,
    const std::vector<input_ctype>& inputs,
    const std::vector<prod_ctype>& prods, size_t num_ev, size_t max_time
) {
    visFileH5::create_file(name, metadata, freqs, inputs, prods, num_ev, max_time);
    setup_raw();
}

visFileH5Fast::~visFileH5Fast() {
    // Save the number of samples added into the `num_time` attribute.
    int nt = (size_t)num_time();
    file->createAttribute<int>(
        "num_time", DataSpace::From(nt)).write(nt);
}


void visFileH5Fast::create_time_axis(size_t max_time) {
    // Fill the time axis with zeros
    std::vector<time_ctype> times(max_time, {0, 0.0});
    create_axis("time", times);
}


void visFileH5Fast::create_dataset(const std::string& name, const std::vector<std::string>& axes,
                                   HighFive::DataType type) {

    std::vector<size_t> dims;

    for(auto axis : axes) {
        dims.push_back(length(axis));
    }

    hid_t create_p = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_layout(create_p, H5D_CONTIGUOUS);
    H5Pset_alloc_time(create_p, H5D_ALLOC_TIME_EARLY);
    H5Pset_fill_time(create_p, H5D_FILL_TIME_NEVER);

    DataSpace space = DataSpace(dims);
    DataSet dset = file->createDataSet(
        name, space, type, create_p
    );
    dset.createAttribute<std::string>(
        "axis", DataSpace::From(axes)).write(axes);
}


void visFileH5Fast::setup_raw() {

    // Get all the dataset lengths
    ntime = 0;
    nfreq = length("freq");
    nprod = length("prod");
    ninput = length("input");
    nev = length("ev");

    // Calculate all the dataset file offsets
    time_offset = H5Dget_offset(dset("index_map/time").getId());
    vis_offset = H5Dget_offset(dset("vis").getId());
    weight_offset = H5Dget_offset(dset("vis_weight").getId());
    gcoeff_offset = H5Dget_offset(dset("gain_coeff").getId());
    gexp_offset = H5Dget_offset(dset("gain_exp").getId());

    if(write_ev) {
        eval_offset = H5Dget_offset(dset("eval").getId());
        evec_offset = H5Dget_offset(dset("evec").getId());
        erms_offset = H5Dget_offset(dset("erms").getId());
    }

    // WARNING: this is very much discouraged by the HDF5 folks. Only really
    // works for the sec2 driver.
    int * fhandle;
    H5Fget_vfd_handle(file->getId(), H5P_DEFAULT, (void**)(&fhandle));
    fd = *fhandle;

#ifndef __APPLE__
    struct stat st;
    if((fstat(fd, &buffer) != 0) || (posix_fallocate(fd, 0, st.st_size) != 0)) {
        ERROR("Couldn't preallocate file: %s", strerror(errno));
    }
#endif
}

template<typename T>
bool visFileH5Fast::write_raw(off_t dset_base, int ind, size_t n, 
                            const std::vector<T>& vec) {


    if(vec.size() < n) {
        ERROR("Expected size of write (%i) exceeds vector length (%i)",
              n, vec.size());
        return false;
    }

    return write_raw(dset_base, ind, n, vec.data());
}

template<typename T>
bool visFileH5Fast::write_raw(off_t dset_base, int ind, size_t n, 
                              const T* data) {


    size_t nb = n * sizeof(T);
    off_t offset = dset_base + ind * nb;

    // Write in a retry macro loop incase the write was interrupted by a signal
    int nbytes = TEMP_FAILURE_RETRY( 
        pwrite(fd, (const void *)data, nb, offset)
    );

    if(nbytes < 0) {
        ERROR("Write error attempting to write %i bytes at offset %i: %s",
              nb, offset, strerror(errno));
        return false;
    }

    return true;
}

uint32_t visFileH5Fast::extend_time(time_ctype new_time) {

    // Perform a raw write of the new time sample
    write_raw(time_offset, ntime, 1, &new_time);

    // Increment the time count and return the index of the added sample
    return ntime++;
}


void visFileH5Fast::write_sample(
    uint32_t time_ind, uint32_t freq_ind, std::vector<cfloat> new_vis,
    std::vector<float> new_weight, std::vector<cfloat> new_gcoeff,
    std::vector<int32_t> new_gexp, std::vector<float> new_eval,
    std::vector<cfloat> new_evec, float new_erms
) {

    write_raw(vis_offset, time_ind * nfreq + freq_ind, nprod, new_vis);
    write_raw(weight_offset, time_ind * nfreq + freq_ind, nprod, new_weight);
    write_raw(gcoeff_offset, time_ind * nfreq + freq_ind, ninput, new_gcoeff);
    write_raw(gexp_offset, time_ind, ninput, new_gexp);

    if(write_ev) {
        write_raw(eval_offset, time_ind * nfreq + freq_ind, nev, new_eval);
        write_raw(evec_offset, time_ind * nfreq + freq_ind, nev * ninput, new_evec);
        write_raw(erms_offset, time_ind * nfreq + freq_ind, 1, (const float*)&new_erms);
    }

    // Figure out what (if any) flushing or advising to do here.
}


size_t visFileH5Fast::num_time() {
    return ntime;
}


// Add support for all our custom types to HighFive
template <> inline DataType HighFive::create_datatype<freq_ctype>() {
    CompoundType f;
    f.addMember("centre", H5T_IEEE_F64LE);
    f.addMember("width", H5T_IEEE_F64LE);
    f.autoCreate();
    return f;
}

template <> inline DataType HighFive::create_datatype<time_ctype>() {
    CompoundType t;
    t.addMember("fpga_count", H5T_STD_U64LE);
    t.addMember("ctime", H5T_IEEE_F64LE);
    t.autoCreate();
    return t;
}

template <> inline DataType HighFive::create_datatype<input_ctype>() {

    CompoundType i;
    hid_t s32 = H5Tcopy(H5T_C_S1);
    H5Tset_size(s32, 32);
    //AtomicType<char[32]> s32;
    i.addMember("chan_id", H5T_STD_U16LE, 0);
    i.addMember("correlator_input", s32, 2);
    i.manualCreate(34);

    return i;
}

template <> inline DataType HighFive::create_datatype<prod_ctype>() {

    CompoundType p;
    p.addMember("input_a", H5T_STD_U16LE);
    p.addMember("input_b", H5T_STD_U16LE);
    p.autoCreate();
    return p;
}

template <> inline DataType HighFive::create_datatype<cfloat>() {
    CompoundType c;
    c.addMember("r", H5T_IEEE_F32LE);
    c.addMember("i", H5T_IEEE_F32LE);
    c.autoCreate();
    return c;
}