#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "cache.h"
#include "jbod.h"

//Uncomment the below code before implementing cache functioncs.
static cache_entry_t *cache = NULL;
static int cache_size = 0;
static int clock = 0;
static int num_queries = 0;
static int num_hits = 0;

//helper function to search cache for entry
//returns index of cache entry with disk_num and block_num, otherwise returns -1 if not found
int cache_search(int disk_num, int block_num){
  for(int i = 0; i < cache_size; i++){
    clock++;
    if(cache[i].valid == false){
      continue;
    }
    if(cache[i].disk_num == disk_num && cache[i].block_num == block_num){
      return i;
    }
  }
  return -1;
}

//helper function to move cache entry at index i to either last valid entry (if cache not full) or last index (if cache full),
//shifting everything in between i and its destination one spot to the left.
//called whenever an entry is inserted or update to maintain order of cache where least recently used are at the front and
//most recently used are at the end
void move_entry(int i){
  //create temp variable holding entry at index i
  cache_entry_t temp = cache[i];
  int j;
  //iterate through loop, shifting each entry 1 spot to the left until reach the end of cache or invalid entry is found
  for(j = i; j < cache_size - 1; j++){
    if(cache[j + 1].valid == false){
      break;
    }
    cache[j] = cache[j + 1];
  }
  //after exiting the loop, i will either be equal to cache_size - 1 or the index of the last valid entry
  //place temp at either the last valid entry (if not full) or end of array (if full)
  cache[j] = temp;
}

int cache_create(int num_entries) {
  //if cache already enabled or num_entries not in valid range, return -1
  if(cache_enabled()){
    return -1;
  }
  if(num_entries < 2 || num_entries > 4096){
    return -1;
  }

  //allocate zero'd out memory for num_entries of cache_entries
  cache = (cache_entry_t *)calloc(num_entries, sizeof(cache_entry_t));
  if(cache == NULL){
    return -1;
  }
  cache_size = num_entries;
  return 1;
}

int cache_destroy(void) {
  //check to make sure cache is enabled, return -1 if not
  if(!cache_enabled()){
    return -1;
  }
  //cache enabled, so free memory, set point to NULL, and set cache_size to 0 and return 1
  free(cache);
  cache = NULL;
  cache_size = 0;
  return 1;
}

int cache_lookup(int disk_num, int block_num, uint8_t *buf) {
  //might need to check if buf is NULL
  //check to make sure there is a cache, return -1 if no cache
  if(!cache_enabled()){
    return -1;
  }
  if(buf == NULL){
    return -1;
  }
  //increment num_queries
  num_queries++;
  //find index of cache entry we are looking for
  int i = cache_search(disk_num, block_num);
  //if entry not in cache, return -1
  if(i == -1){
    return -1;
  }
  //entry in cache, so increment num_hits, copy its block into buffer, update timestamp, and return 1
  num_hits++;
  memcpy(buf, cache[i].block, JBOD_BLOCK_SIZE);
  cache[i].clock_accesses = clock;
  //move cache entry to end of array since most recently used
  move_entry(i);
  return 1;
}

void cache_update(int disk_num, int block_num, const uint8_t *buf) {
  //check to make sure there is a cache, return -1 if no cache
  if(!cache_enabled()){
    return;
  }
  if(buf == NULL){
    return;
  }
  //find index of cache entry we are looking for
  int i = cache_search(disk_num, block_num);
  //if entry not in cache, return -1
  if(i == -1){
    return;
  }
  //entry in cache, copy buf into its block, update timestamp, and return 1
  memcpy(cache[i].block, buf, JBOD_BLOCK_SIZE);
  cache[i].clock_accesses = clock;
  //move cache entry to end of array since most recently used
  move_entry(i);
  return;
}

int cache_insert(int disk_num, int block_num, const uint8_t *buf) {
  //check to make sure there is a cache, return -1 if no cache
  if(!cache_enabled()){
    return -1;
  }
  if(buf == NULL){
    return -1;
  }
  if(disk_num < 0 || disk_num > JBOD_NUM_DISKS || block_num < 0 || block_num > JBOD_NUM_BLOCKS_PER_DISK){
    return -1;
  }
  //if passed entry already in cache, update if buf is different than its block, if buf is same as its block, do nothing
  int j = cache_search(disk_num, block_num);
  //if passed entry in cache
  if(j != -1){
    //if buf is equal to passed entry's current block, do nothing and return -1
    if(memcmp(cache[j].block, buf, JBOD_BLOCK_SIZE) == 0){
      return -1;
    }
    //if here, passed entry is in cache, however buf is not equal to its block, so update its block to buf
    cache_update(disk_num, block_num, buf);
    return 1;
  }
  //find first empty cache index, or, if cache full, last index which will be most recently used
  int i;
  for(i = 0; i < cache_size - 1; i++){
    clock++;
    if(cache[i].valid == false){
      break;
    }
  }
  //replace most recently used entry with values passed to function
  cache[i].disk_num = disk_num;
  cache[i].block_num = block_num;
  cache[i].clock_accesses = clock;
  cache[i].valid = true;
  memcpy(cache[i].block, buf, JBOD_BLOCK_SIZE);
  if(i != cache_size - 1){
    move_entry(i);
  }
  return 1;
}

bool cache_enabled(void) {
  return cache != NULL;
}

void cache_print_hit_rate(void) {
	fprintf(stderr, "num_hits: %d, num_queries: %d\n", num_hits, num_queries);
  fprintf(stderr, "Hit rate: %5.1f%%\n", 100 * (float) num_hits / num_queries);
}


//cache is ordered so that elements are in order of recent use with least recently used being at index 0 and most recently
//used being at either the last index (if cache is full) or the last valid entry (if cache is not full), so when reallocing
//to smaller size, the end of cache (most recently used entries or invalid entries) will automatically be the entries removed
int cache_resize(int new_num_entries) {
  //check to make sure there is a cache, return -1 if no cache
  if(!cache_enabled()){
    return -1;
  }
  if(new_num_entries < 2 || new_num_entries > 4096){
    return -1;
  }
  //attempt realloc, store result in  *ptr
  cache_entry_t *ptr = (cache_entry_t *)realloc(cache, new_num_entries * sizeof(cache_entry_t));
  //if ptr is NULL, realloc must have failed, so return -1
  if(ptr == NULL){
    return -1;
  }
  //if here, realloc successful, set cache to ptr returned by realloc
  cache = ptr;

  //if new size is bigger, insert invalid (zero'd out) entries
  if(new_num_entries > cache_size){
    //need to insert new_num_entries - cache_size number of new, invalid entries (zero out their memory)
    memset(&cache[cache_size], 0, (new_num_entries - cache_size) * sizeof(cache_entry_t));
  }

  //update cache_size to new size
  cache_size = new_num_entries;
  return 1;
}