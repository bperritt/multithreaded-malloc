#include <sys/mman.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>

#include "xmalloc.h"

static long sizes[8] = {16, 32, 64, 128, 256, 512, 1024, 2048};
static long page_nums[8] = {4, 8, 16, 32, 64, 128, 256, 512};
static arena arenas[8];
static int init = 0;
const size_t PAGE_SIZE = 4096;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

// initialize arenas during first malloc
void
init_arenas()
{
    for (int ii = 0; ii < 8; ++ii) {
         arenas[ii].id = ii;
         arenas[ii].in_use = 0;
         pthread_mutex_init(&(arenas[ii].lock), 0);
    }
    init = 1;
}

// pick the first available arena to allocate memory to, otherwise choose 0 and wait
arena*
choose_arena()
{
    for (int ii = 0; ii < 8; ++ii) {
        if (!arenas[ii].in_use) {
            arenas[ii].in_use = 1;
            return &(arenas[ii]);
        }
    }
    return &(arenas[0]);
}

void
xfree(void* ap)
{
    // subtract 8 bytes to get offset
    ap = (char*) ap - 8;
    size_t offset = *((size_t*) ap);

    // find the head of the bucket and the size of the bucket
    bucket_header* bucket = (bucket_header*) ((char*) ap - offset);
    size_t size = bucket->size;

    // if it's a large mapping, just unmap
    if (size > 2048) {
        munmap(ap, size);
    } else {
        // get the correct arena from the bucket
        arena* a = &(arenas[bucket->arena]);
        // lock the arena
        pthread_mutex_lock(&(a->lock));
        // the index of the entry is the offset divided by the size, set it to 0 and reduce the fill
        int index = offset / size;
        bucket->bitmap[index] = 0;
        bucket->fill--;
        // calculate the number of bitmap spots taken by metadata
        long divs = sizeof(bucket_header) / size;
        divs = sizeof(bucket_header) % size == 0 ? divs : divs + 1;
        // if the bucket only has metadata in it
        if (bucket->fill == divs) {
            // find the index of the array
            int bucket_num = 0;
            while (sizes[bucket_num] != size) {
                bucket_num++;
            }
            a->bucket_head[bucket_num] = 0;
            // unmap the bucket
            munmap(bucket, PAGE_SIZE * page_nums[bucket_num]);
        }
        // unlock the thread
        pthread_mutex_unlock(&(a->lock));
    }
}

// allocate a new page of size
void*
newpage(size_t size)
{
    return mmap(0, size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
}  

bucket_header*
page_setup(int index, arena* a)
{
    // find the correct size an map a large enough set of pages
    bucket_header* bucket;
    size_t size = sizes[index];
    bucket = (bucket_header*) newpage(PAGE_SIZE * page_nums[index]);

    // find how many bitmap entries will be taken by metadata
    long divs = sizeof(bucket_header) / size;
    divs = sizeof(bucket_header) % size == 0 ? divs : divs + 1;

    // set the metadata accordingly
    bucket->size = size;
    bucket->arena = a->id;
    bucket->fill = divs;

    // fill divs entries with 1s
    for (int ii = 0; ii < divs; ++ii) {
        bucket->bitmap[ii] = 1;
    }
    // fill the others with 0s
    for (int ii = divs; ii < 1024; ++ii) {
        bucket->bitmap[ii] = 0;
    }
    return bucket;
}

void*
get_block(bucket_header* bucket, int bucket_num, arena* a)
{
    // find an empty block using the bitmap
    int ii;
    for (ii = 0; ii < 1024; ++ii) {
        if (bucket->bitmap[ii] == 0) {
            bucket->bitmap[ii] = 1;
            break;
        }
    }  
    // increase bucket fill factor
    bucket->fill++;
    // if the bucket is full, remove it from the arena
    if (bucket->fill == 1024) {
        a->bucket_head[bucket_num] = 0;
    }    
    // calculate the offset from the bottom of the bucket, move the data up, and append the offset
    size_t bit_offset = bucket->size * ii;
    void* data = (void*) bucket + bit_offset;
    memcpy(data, &bit_offset, 8);
    return data;
}

void*
xmalloc(size_t nbytes)
{   
    // initialize arenas
    if (!init) {
        init_arenas();
    }
    // lock globally to choose an arena
    pthread_mutex_lock(&global_lock);
    arena* a = choose_arena();
    pthread_mutex_unlock(&global_lock);
    pthread_mutex_lock(&(a->lock));
    // lock the specific arena
    nbytes += 8;
    // if it's a big allocation, add the size to the bottom, set offset as 8, and unlock mutex
    if (nbytes > 2048) {
        nbytes += 8;
        void* data = newpage(nbytes);
        memcpy(data, &nbytes, 8); 
        size_t offset = 8;
        memcpy((char*) data + 8, &offset, 8);  
        a->in_use = 0;
        pthread_mutex_unlock(&(a->lock));
        return (char*) data + 16;    
    } else {
        // otherwise, find the correct size partition for the allocation
        int index = 0;
        while (sizes[index] < nbytes) {
            index++;
        }
        // get the bucket at the front of the freelist. If it doesn't exist, make it
        bucket_header* bucket = a->bucket_head[index];
        if (bucket == 0) {
            a->bucket_head[index] = page_setup(index, a);
        }
        // get a chunk of available data
        void* data = get_block(a->bucket_head[index], index, a);
        // free up the arena and return the data
        a->in_use = 0;
        pthread_mutex_unlock(&(a->lock));
        return (char*) data + 8; 
    }       
}

void*
xrealloc(void* prev, size_t nn)
{
    // malloc, move data, free
    void* new_data = xmalloc(nn);
    memcpy(new_data, prev, nn); 
    xfree(prev);
    return new_data;
}
