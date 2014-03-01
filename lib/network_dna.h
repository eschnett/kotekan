#ifndef NETWORK
#define NETWORK

#include "buffers.h"
#include "errors.h"

struct network_thread_arg {
    char * ip_address;
    struct Buffer * buf;
    int portNumber;
    int bufferDepth;
    int data_limit;
    int numLinks;
    int link_id;

    // Args used for testing.
    int num_timesamples;
    int actual_num_freq;
    int actual_num_elements;
};

void network_thread(void * arg);

void test_network_thread(void * arg);

#endif