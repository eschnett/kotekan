
/**
 * @file
 * @Merge the raw buffer to a buffer with longer timespan.
 *  - mergeRawBuffer : public kotekan::Stage
 */

#ifndef MERGE_RAW_BUFFER
#define MERGE_RAW_BUFFER

#include "Config.hpp" // for Config
#include "Stage.hpp"
#include "bufferContainer.hpp" // for bufferContainer
#include "BeamMetadata.hpp"   // for BeamMetadata

#include <stdint.h> // for uint32_t
#include <string>   // for string
#include <vector>   // for vector


using std::vector;


/**
 * @class mergeRawBuffer
 * @brief The  
 *
 * @par Buffers
 * @buffer in_buf Kotekan buffer of raw packets.
 *     @buffer_format Array of @c chars
 * @buffer out_buf Kotekan buffer of compressed lost samples.
 *     @buffer_format Array of @c uint32_t
 *
 * @conf   samples_per_data_set  Int.    No. of samples.
 * @conf   compression_factor    Int.    Number of samples to group.
 *
 * @author Jing
 *
 *
 */



class mergeRawBuffer : public kotekan::Stage {
public:
    /// Constructor 
    mergeRawBuffer(kotekan::Config& config_, const std::string& unique_name,
                    kotekan::bufferContainer& buffer_container);
    /// Destructor
    virtual ~mergeRawBuffer();
    /// Primary loop to wait for buffers, put package together.
    void main_thread() override;

private:
    /// Merged buffer for the merged
    struct Buffer* in_buf;

    /// Frame merged buffer 
    struct Buffer* out_buf;


    /// Config variables
    uint32_t _samples_per_data_set;
    uint32_t _raw_frames_per_merged_frame;
    uint32_t _num_pol;
};


#endif // MERGE_RAW_BUFFER_HPP
