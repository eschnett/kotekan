#include "nDiskFileRead.hpp"
#include <random>
#include "errors.h"
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include "vdif_functions.h"

using std::string;

nDiskFileRead::nDiskFileRead(Config& config, const string& unique_name,
                                bufferContainer &buffer_containter) :
    KotekanProcess(config, unique_name, buffer_containter,
                     std::bind(&nDiskFileRead::main_thread, this))
{
    //Get variables from config
    apply_config(0);

    //Mark as producer
    register_producer(buf, unique_name.c_str());
}

nDiskFileRead::~nDiskFileRead() {

}

void nDiskFileRead::apply_config(uint64_t fpga_seq) {

    buf = get_buffer("out_buf"); //Buffer

    //Data paramters
    num_disks = config.get_int(unique_name,"num_disks"); 

    //Data location parameters
    disk_base = config.get_string(unique_name,"disk_base"); 
    disk_set = config.get_string(unique_name,"disk_set");
    capture = config.get_string(unique_name,"capture");
    starting_index = config.get_int(unique_name,"starting_file_index");
}

void nDiskFileRead::main_thread() {

    // Create the threads
    file_thread_handles.resize(num_disks);
    for (uint32_t i = 0; i < num_disks; ++i) {
        file_thread_handles[i] = std::thread(&nDiskFileRead::file_read_thread, this, i);

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        INFO("Setting thread affinity");
        for (auto &i : config.get_int_array(unique_name, "cpu_affinity"))
            CPU_SET(i, &cpuset);

        pthread_setaffinity_np(file_thread_handles[i].native_handle(), sizeof(cpu_set_t), &cpuset);
    }

    // Join the threads
    for (uint32_t i = 0; i < num_disks; ++i) {
        file_thread_handles[i].join();
    }
}

void nDiskFileRead::file_read_thread(int disk_id) {

    unsigned int buf_id = disk_id;
    //Starting File index
    unsigned int file_index = disk_id + starting_index; 
    INFO("%s%s/%d/%s/%07d.vdif",disk_base.c_str(), disk_set.c_str(), 
                                                disk_id,
                                                capture.c_str(),
                                                file_index)
    //Endless loop
    while (!stop_thread) { 

        unsigned char* buf_ptr = (unsigned char*) wait_for_empty_frame(buf, unique_name.c_str(), buf_id);
        if (buf_ptr == NULL) break;

        char file_name[100]; //Find current file
        snprintf(file_name, sizeof(file_name), "%s%s/%d/%s/%07d.vdif", 
                                                disk_base.c_str(), 
                                                disk_set.c_str(), 
                                                disk_id,
                                                capture.c_str(),
                                                file_index);
        
        //Open current file for reading
        FILE * in_file = fopen(file_name, "r"); 

        //Make Sure file is the right size
        fseek(in_file, 0L, SEEK_END); 
        long sz = ftell(in_file);
        rewind(in_file);

        if(sz != buf->frame_size){
            INFO("File size %ld Frame Size %d", sz, buf->frame_size);
        }
        assert(sz == buf->frame_size);

        //Read into buffer
	fread(buf_ptr,buf->frame_size,1,in_file);
        fclose(in_file);
        INFO("%s Read Complete Marking Frame ID %d Full\n", file_name, buf_id);
        
        mark_frame_full(buf, unique_name.c_str(), buf_id);

        //Go to next file
        buf_id = (buf_id + num_disks) % buf->num_frames;
        file_index += num_disks;
        
    }
}
