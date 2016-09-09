#include "device_interface.h"
#include "gpu_command.h"
#include "callbackdata.h"
#include "math.h"
#include <errno.h>

device_interface::device_interface(struct Buffer * param_In_Buf, struct Buffer * param_Out_Buf, Config * param_Config
, int param_GPU_ID, struct Buffer * param_beamforming_out_buf, struct Buffer * param_beamforming_out_incoh_buf)
{
    cl_int err;

    in_buf = param_In_Buf;
    out_buf = param_Out_Buf;
    config = param_Config;
    gpu_id = param_GPU_ID;
    beamforming_out_buf = param_beamforming_out_buf;
    beamforming_out_incoh_buf = param_beamforming_out_incoh_buf;
//    use_beamforming = param_Config->gpu.use_beamforming;

      // TODO explain these numbers/formulas.
    num_blocks = (param_Config->processing.num_adjusted_elements / param_Config->gpu.block_size) *
    (param_Config->processing.num_adjusted_elements / param_Config->gpu.block_size + 1) / 2.;

    accumulate_len = config->processing.num_adjusted_local_freq *
    config->processing.num_adjusted_elements * 2 * config->processing.num_data_sets * sizeof(cl_int);
    aligned_accumulate_len = PAGESIZE_MEM * (ceil((double)accumulate_len / (double)PAGESIZE_MEM));
    assert(aligned_accumulate_len >= accumulate_len);

//    stream_info = malloc(in_buf->num_buffers * sizeof(struct StreamINFO));
//    CHECK_MEM(stream_info);

    // Get a platform.
    CHECK_CL_ERROR( clGetPlatformIDs( 1, &platform_id, NULL ) );

    // Find a GPU device..
    CHECK_CL_ERROR( clGetDeviceIDs( platform_id, CL_DEVICE_TYPE_GPU, MAX_GPUS, device_id, NULL) );

    context = clCreateContext( NULL, 1, &device_id[gpu_id], NULL, NULL, &err);
    CHECK_CL_ERROR(err);

    //Used for beamforming. Should this logic live in one of the beamforming command_objects?
    //LOOK AT SETTING StreamID for all Num_buffers.
    //device.set_stream_info(bufferID);
    /*
    for (int i = 0; i < in_buf->num_buffers; ++i) {
        // Set the stream ID for the link.
        int32_t local_stream_id = get_streamID(in_buf, i);
        assert(local_stream_id != -1);
        stream_info[i].stream_id = extract_stream_id(local_stream_id);
    }
    */
}

int device_interface::getNumBlocks()
{
    return num_blocks;
}

int device_interface::getGpuID()
{
    return gpu_id;
}

//int device_interface::get_use_beamforming()
//{
//    return use_beamforming;
//}

Buffer* device_interface::getInBuf()
{
    return in_buf;
}

Buffer* device_interface::getOutBuf()
{
    return out_buf;
}

Buffer* device_interface::get_beamforming_out_buf()
{
    return beamforming_out_buf;
}

Buffer* device_interface::get_beamforming_out_incoh_buf()
{
    return beamforming_out_incoh_buf;
}

cl_context device_interface::getContext()
{
    return context;
}

cl_device_id device_interface::getDeviceID(int param_GPUID)
{
    return device_id[param_GPUID];
}

cl_int* device_interface::getAccumulateZeros()
{
    return accumulate_zeros;
}

int device_interface::getAlignedAccumulateLen() const
{
    return aligned_accumulate_len;
}

//void device_interface::set_stream_id(int param_buffer_id)
//{
//    // Set the stream ID for the link.
//    int32_t local_stream_id = get_streamID(in_buf, param_buffer_id);
//    assert(local_stream_id != -1);
//    stream_id = extract_stream_id(local_stream_id);
//}
//
//stream_id_t device_interface::get_stream_id()
//{
//    return stream_id;
//}

//void device_interface::set_device_phases(cl_mem param_device_phases)
//{
//    device_phases = param_device_phases;
//}

void device_interface::prepareCommandQueue()
{
    cl_int err;

    // Create command queues
    for (int i = 0; i < NUM_QUEUES; ++i) {
        queue[i] = clCreateCommandQueue( context, device_id[gpu_id], 0, &err );
        CHECK_CL_ERROR(err);
    }
}

void device_interface::allocateMemory()
{
    //IN THE FUTURE, ANDRE TALKED ABOUT WANTING MEMORY TO BE DYNAMICALLY ALLOCATE BASED ON KERNEL STATES AND ASK FOR MEMORY BY SIZE AND HAVE THAT MEMORY NAMED BY THE KERNEL.
    //KERNELS WOULD THEN BE PASSED INTO DEVICE_INTERFACE
    //TO BE GIVEN MEMORY AND DEVICE_INTERFACE WOULD LOOP THROUGH THE KERNELS MEMORY STATES TO DETERMINE ALL THE MEMORY THAT KERNEL NEEDS.


    cl_int err;
    // TODO create a struct to contain all of these (including events) to make this memory allocation cleaner.

    // Setup device input buffers
    device_input_buffer = (cl_mem *) malloc(in_buf->num_buffers * sizeof(cl_mem));
    CHECK_MEM(device_input_buffer);
    for (int i = 0; i < in_buf->num_buffers; ++i) {
        device_input_buffer[i] = clCreateBuffer(context, CL_MEM_READ_ONLY, in_buf->aligned_buffer_size, NULL, &err);
        CHECK_CL_ERROR(err);
    }

    // Array used to zero the output memory on the device.
    // TODO should this be in it's own function?
    err = posix_memalign((void **) &accumulate_zeros, PAGESIZE_MEM, aligned_accumulate_len);
    if ( err != 0 ) {
        ERROR("Error creating aligned memory for accumulate zeros");
        exit(err);
    }

    // Ask that all pages be kept in memory
    err = mlock((void *) accumulate_zeros, aligned_accumulate_len);
    if ( err == -1 ) {
        ERROR("Error locking memory - check ulimit -a to check memlock limits");
        exit(errno);
    }
    memset(accumulate_zeros, 0, aligned_accumulate_len );

    // Setup device accumulate buffers.
    device_accumulate_buffer = (cl_mem *) malloc(in_buf->num_buffers * sizeof(cl_mem));
    CHECK_MEM(device_accumulate_buffer);
    for (int i = 0; i < in_buf->num_buffers; ++i) {
        device_accumulate_buffer[i] = clCreateBuffer(context, CL_MEM_READ_WRITE, aligned_accumulate_len, NULL, &err);
        CHECK_CL_ERROR(err);
    }

    // Setup device output buffers.
    device_output_buffer = (cl_mem *) malloc(out_buf->num_buffers * sizeof(cl_mem));
    CHECK_MEM(device_output_buffer);
    for (int i = 0; i < out_buf->num_buffers; ++i) {
        device_output_buffer[i] = clCreateBuffer(context, CL_MEM_WRITE_ONLY, out_buf->aligned_buffer_size, NULL, &err);
        CHECK_CL_ERROR(err);
    }



////##OCCURS IN SETUP_OPEN_CL
    // Setup beamforming output buffers.
    if (config->gpu.use_beamforming == 1) {
        device_beamform_output_buffer = (cl_mem *) malloc(beamforming_out_buf->num_buffers * sizeof(cl_mem));
        CHECK_MEM(device_beamform_output_buffer);
        for (int i = 0; i < beamforming_out_buf->num_buffers; ++i) {
            device_beamform_output_buffer[i] = clCreateBuffer(context, CL_MEM_WRITE_ONLY
                    , beamforming_out_buf->aligned_buffer_size, NULL, &err);
            CHECK_CL_ERROR(err);
        }
        
        device_beamform_output_incoh_buffer = (cl_mem *) malloc(beamforming_out_incoh_buf->num_buffers * sizeof(cl_mem));
        CHECK_MEM(device_beamform_output_incoh_buffer);
        for (int i = 0; i < beamforming_out_incoh_buf->num_buffers; ++i) {
            device_beamform_output_incoh_buffer[i] = clCreateBuffer(context, CL_MEM_WRITE_ONLY
                    , beamforming_out_incoh_buf->aligned_buffer_size, NULL, &err);
            CHECK_CL_ERROR(err);
        }

        // We have two phase blanks
        const int num_phase_blanks = 2;
        device_phases = (cl_mem *) malloc(num_phase_blanks * sizeof(cl_mem));
        CHECK_MEM(device_phases);
        for (int i = 0; i < num_phase_blanks; ++i) {
            device_phases[i] = clCreateBuffer(context, CL_MEM_READ_ONLY, config->processing.num_elements * sizeof(float)
                    , NULL, &err);
            CHECK_CL_ERROR(err);
        }
    }
////##

}

cl_mem device_interface::getInputBuffer(int param_BufferID)
{
    return device_input_buffer[param_BufferID];
}

cl_mem device_interface::getOutputBuffer(int param_BufferID)
{
    return device_output_buffer[param_BufferID];
}

cl_mem device_interface::getAccumulateBuffer(int param_BufferID)
{
  return device_accumulate_buffer[param_BufferID];
}

cl_mem device_interface::get_device_beamform_output_buffer(int param_BufferID)
{
    return device_beamform_output_buffer[param_BufferID];
}

cl_mem device_interface::get_device_beamform_output_incoh_buffer(int param_BufferID)
{
    return device_beamform_output_incoh_buffer[param_BufferID];
}

cl_mem device_interface::get_device_phases(int param_bankID)
{
    return device_phases[param_bankID];
}

cl_mem device_interface::get_device_freq_map(int32_t encoded_stream_id)
{
    std::map<int32_t, cl_mem>::iterator it = device_freq_map.find(encoded_stream_id);
    if(it == device_freq_map.end())
    {
        // Create the freq map for the first time.
        cl_int err;
        stream_id_t stream_id = extract_stream_id(encoded_stream_id);
        uint32_t num_local_freq = config->processing.num_local_freq;
        float freq[num_local_freq];

        for (int j = 0; j < num_local_freq; ++j) {
            freq[j] = freq_from_bin(bin_number(&stream_id, j))/1000.0;
        }

        device_freq_map[encoded_stream_id] = clCreateBuffer(context,
                                            CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                            num_local_freq * sizeof(float), freq, &err);
        CHECK_CL_ERROR(err);
    }
    return device_freq_map[encoded_stream_id];
}

void device_interface::deallocateResources()
{
    cl_int err;

    for (int i = 0; i < NUM_QUEUES; ++i) {
        CHECK_CL_ERROR( clReleaseCommandQueue(queue[i]) );
    }

    for (int i = 0; i < in_buf->num_buffers; ++i) {
        CHECK_CL_ERROR( clReleaseMemObject(device_input_buffer[i]) );
    }
    free(device_input_buffer);

    for (int i = 0; i < in_buf->num_buffers; ++i) {
        CHECK_CL_ERROR( clReleaseMemObject(device_accumulate_buffer[i]) );
    }
    free(device_accumulate_buffer);

    for (int i = 0; i < out_buf->num_buffers; ++i) {
        CHECK_CL_ERROR( clReleaseMemObject(device_output_buffer[i]) );
    }
    free(device_output_buffer);

    if (config->gpu.use_beamforming == 1) {

        for (int i = 0; i < 2; ++i) {
            CHECK_CL_ERROR( clReleaseMemObject(device_phases[i]) );
        }

        for (int i = 0; i < beamforming_out_buf->num_buffers; ++i) {
            CHECK_CL_ERROR( clReleaseMemObject(device_beamform_output_buffer[i]) );
        }
        free(device_beamform_output_buffer);

        for (int i = 0; i < beamforming_out_incoh_buf->num_buffers; ++i) {
            CHECK_CL_ERROR( clReleaseMemObject(device_beamform_output_incoh_buffer[i]) );
        }
        free(device_beamform_output_incoh_buffer);
        
        for (std::map<int32_t,cl_mem>::iterator it=device_freq_map.begin(); it!=device_freq_map.end(); ++it){
            CHECK_CL_ERROR( clReleaseMemObject(it->second) );
        }
    }

    err = munlock((void *) accumulate_zeros, aligned_accumulate_len);
    if ( err == -1 ) {
        ERROR("Error unlocking memory");
        exit(errno);
    }
    free(accumulate_zeros);

    CHECK_CL_ERROR( clReleaseContext(context) );

}

cl_command_queue device_interface::getQueue(int param_Dim)
{
    return queue[param_Dim];
}


