#ifndef VIS_FILE_HPP
#define VIS_FILE_HPP

#include <iostream>
#include <map>
#include "errors.h"
#include <highfive/H5File.hpp>
#include <highfive/H5DataSet.hpp>

// Structs to represent the datatypes of the index maps
struct freq_ctype {
    double centre;
    double width;
};

struct input_ctype {
    // Allow initialisation from a std::string
    input_ctype(uint16_t id, std::string serial);

    uint16_t chan_id;
    char correlator_input[32];
};

struct time_ctype {
    uint64_t fpga_count;
    double ctime;
};

struct prod_ctype {
    uint16_t input_a;
    uint16_t input_b;
};

struct complex_int {
    int32_t r;
    int32_t i;
};


/// Class to manage access to a CHIME correlator file
///
/// This is only designed with writing data in mind.
class visFile {

public:

    /// Create file (and lock file).
    /// \param name Name of the file to write
    /// \param acq_name Name of the acquisition to write
    /// \param inst_name Instrument name (e.g. chime)
    /// \param notes Note about the acquisition
    /// \param freqs Frequencies channels that will be in the file
    /// \param inputs Inputs that are in the file
    visFile(const std::string& name,
            const std::string& acq_name,
            const std::string& inst_name,
            const std::string& notes,
            const std::vector<freq_ctype>& freqs,
            const std::vector<input_ctype>& inputs);
    ~visFile();


    /// Write a new time sample into this file
    /// \param new_time Time of sample
    /// \param freq_ind Index of the frequency we are writing
    /// \param new_vis Visibility data for this frequency
    /// \param new_weight Visibility weights for this frequency
    /// \param new_gcoeff Gain coefficient data
    /// \param new_gexp Gain exponent data
    /// \return The number of entries in the time axis
    size_t addSample(time_ctype new_time, uint32_t freq_ind,
                     std::vector<complex_int> new_vis,
                     std::vector<uint8_t> new_weight,
                     std::vector<complex_int> new_gcoeff,
                     std::vector<int32_t> new_gexp);

    uint32_t extendTime(time_ctype new_time);


    void writeSample(uint32_t time_ind, uint32_t freq_ind,
                     std::vector<complex_int> new_vis,
                     std::vector<uint8_t> new_weight,
                     std::vector<complex_int> new_gcoeff,
                     std::vector<int32_t> new_gexp);

    size_t num_time();

private:


    // Create the index maps from the frequencies and the inputs
    void createIndex(const std::vector<freq_ctype>& freqs,
                     const std::vector<input_ctype>& inputs);

    // Create the main visibility holding datasets
    void createDatasets(size_t nfreq, size_t ninput, size_t nprod);

    // Get datasets
    HighFive::DataSet vis();
    HighFive::DataSet vis_weight();
    HighFive::DataSet gain_coeff();
    HighFive::DataSet gain_exp();
    HighFive::DataSet time();

    // Get dimensions
    size_t num_prod();
    size_t num_input();
    size_t num_freq();

    // Pointer to the underlying HighFive file
    std::unique_ptr<HighFive::File> file;

    std::string lock_filename;

};


/// This container holds the correlator files that are being actively written to.
/// This is only designed with writing data in mind.
class visFileBundle {

public:

    /// Initialise the file bundle
    /// \param acq_name Name of the acquisition to write
    /// \param freq_chunk ID of the frequency chunk being written
    /// \param inst_name Instrument name (e.g. chime)
    /// \param notes Note about the acquisition
    /// \param freqs Frequencies channels that will be in the file
    /// \param inputs Inputs that are in the file
    visFileBundle(const std::string acq_name,
                  int freq_chunk,
                  const std::string instrument_name,
                  const std::string notes,
                  const std::vector<freq_ctype>& freqs,
                  const std::vector<input_ctype>& inputs,
                  size_t rollover=1024, size_t window_size=10);
    //~visFileBundle();


    /// Write a new time sample into this set of files
    /// \param new_time Time of sample
    /// \param freq_ind Index of the frequency we are writing
    /// \param new_vis Visibility data for this frequency
    /// \param new_weight Visibility weights for this frequency
    /// \param new_gcoeff Gain coefficient data
    /// \param new_gexp Gain exponent data
    /// \return The number of entries in the time axis
    void addSample(time_ctype new_time, uint32_t freq_ind,
                   std::vector<complex_int> new_vis,
                   std::vector<uint8_t> new_weight,
                   std::vector<complex_int> new_gcoeff,
                   std::vector<int32_t> new_gexp);

private:

    void addFile(time_ctype first_time);

    const std::string root_path;
    const int freq_chunk;

    const std::string instrument_name;
    const std::string notes;

    const std::vector<freq_ctype>& freqs;
    const std::vector<input_ctype>& inputs;

    size_t rollover;
    size_t window_size;

    std::string acq_name;
    double acq_start_time;

    std::map<uint64_t, std::tuple<std::shared_ptr<visFile>, uint32_t>> vis_file_map;

};


// These templated functions are needed in order to tell HighFive how the
// various structs are converted into HDF5 datatypes
namespace HighFive {
template <> DataType create_datatype<freq_ctype>();
template <> DataType create_datatype<time_ctype>();
template <> DataType create_datatype<input_ctype>();
template <> DataType create_datatype<prod_ctype>();
template <> DataType create_datatype<complex_int>();
};


#endif
