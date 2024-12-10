#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "jbod.h"
#include "mdadm.h"
//keep this include below? wasn't included in repo I wrote it
#include "net.h"

int mounted = 0;
int has_write_permission = 0;

//function to build uint32_t op to pass to jbod_operation
uint32_t buildOperation(uint32_t diskID, uint32_t blockID, uint32_t command){
  uint32_t retval = 0x0, tempa, tempb, tempc, tempd;

  tempa = (diskID & 0xff);
  tempb = (blockID & 0xff) << 4;
  tempc = (command & 0xff) << 12;
  tempd = (0x0 & 0xffff) << 20;

  retval = tempa | tempb | tempc | tempd;

  return retval;
}

//helper functions to find current disk and current block given start address
int getCurrentDisk(int addr){
  return addr / JBOD_DISK_SIZE;
}
int getCurrentBlock(int addr){
  return (addr % JBOD_DISK_SIZE) / JBOD_BLOCK_SIZE;
}

//helper function to get the number of blocks needed spanned by a start address and length
int numBlocksCovered(int addr, int length){
  int end_addr = addr + length - 1;
  int current_disk = getCurrentDisk(addr);
  int current_block = getCurrentBlock(addr);
  int future_disk = getCurrentDisk(end_addr);
  int future_block = getCurrentBlock(end_addr);

  return ((future_disk * JBOD_NUM_BLOCKS_PER_DISK) + future_block) - ((current_disk * JBOD_NUM_BLOCKS_PER_DISK) + current_block) + 1;

}

int mdadm_mount(void) {
	if(mounted){
    return -1;
  }

  jbod_client_operation(buildOperation(0, 0, JBOD_MOUNT), NULL);
  mounted = 1;
  return 1;

  /*
  if(jbod_client_operation(buildOperation(0, 0, JBOD_MOUNT), NULL)){
    mounted = 1;
    return 1;
  }
  return -1;
  */
}

int mdadm_unmount(void) {
  if(!mounted){
    return -1;
  }
  if(jbod_client_operation(buildOperation(0, 0, JBOD_UNMOUNT), NULL) == 0){
    mounted = 1;
    return 1;
  }
  return -1;
}

int mdadm_write_permission(void){
  jbod_client_operation(buildOperation(0, 0, JBOD_WRITE_PERMISSION), NULL);
  has_write_permission = 1;
  return 1;
  /*if(jbod_client_operation(buildOperation(0, 0, JBOD_WRITE_PERMISSION), NULL) == 1){
    has_write_permission = 1;
    return 1;
  }
  return -1;*/
}


int mdadm_revoke_write_permission(void){
  if(!has_write_permission){
    return -1;
  }
  if(jbod_client_operation(buildOperation(0, 0, JBOD_REVOKE_WRITE_PERMISSION), NULL) == 1){
    has_write_permission = 0;
    return 1;
  }
  return -1;
}


int mdadm_read(uint32_t addr, uint32_t len, uint8_t *buf) {
  //check to make sure mounted, read_len doesn't exceed 1024 bytes, and won't read beyond valid address space
  if(!mounted){
    return -3;
  }
  if(len > 1024){
    return -2;
  }
  if(addr + len > JBOD_NUM_DISKS * JBOD_DISK_SIZE){
    return -1;
  }
  if(buf == NULL && len != 0){
    return -4;
  }

  //get current disk and block where start_addr is located
  int current_disk = getCurrentDisk(addr);
  int current_block = getCurrentBlock(addr);

  //set current I/O position to current disk and block
  jbod_client_operation(buildOperation(current_disk, 0, JBOD_SEEK_TO_DISK), NULL);
  jbod_client_operation(buildOperation(0, current_block, JBOD_SEEK_TO_BLOCK), NULL);

  //only 256 bytes per block but read_len can be up to 1024 bytes so many need to read multiple blocks
  //determine number of blocks to read
  int num_blocks_to_read = numBlocksCovered(addr, len);

  //create temporary array to read all block data to when calling JBOD_READ_BLOCK
  uint8_t read_block_data_to[num_blocks_to_read * JBOD_BLOCK_SIZE];

  //need to read num_blocks_to_read times since JBOD_READ_BLOCK operation only reads one block at a time
  for(int i = 0; i < num_blocks_to_read; i++){
    //check if next block is on next disk, if it is, seek to next disk and its first block
    if(current_block >= JBOD_NUM_BLOCKS_PER_DISK){
      current_disk++;
      current_block = 0;
      jbod_client_operation(buildOperation(current_disk, 0, JBOD_SEEK_TO_DISK), NULL);
      jbod_client_operation(buildOperation(0, current_block, JBOD_SEEK_TO_BLOCK), NULL);
    }

    //check if block we are looking for is in cache, calling cache_lookup and passing buffer. if cache_lookup returns -1, block
    //was not in cache, so we must read normally and then insert it into the cache. If cache_lookup does not return -1, block
    //was in the cache and was copied to read_block_data_to when cache_lookup was called, so we do not need to read block
    if(cache_lookup(current_disk, current_block, &read_block_data_to[JBOD_BLOCK_SIZE * i]) == -1){
      jbod_client_operation(buildOperation(0, 0, JBOD_READ_BLOCK), &read_block_data_to[JBOD_BLOCK_SIZE * i]);
      cache_insert(current_disk, current_block, &read_block_data_to[JBOD_BLOCK_SIZE * i]);
    }
    current_block++;
  }

  //determine where in blocks read addr begins, and copy from there into buf
  int overflow = (addr % JBOD_DISK_SIZE) % JBOD_BLOCK_SIZE;
  memcpy(buf, &read_block_data_to[overflow], len);

  //read was successful, so return length read
  return len;


}

int mdadm_write(uint32_t addr, uint32_t len, const uint8_t *buf) {
  //make sure has write permission, system is mounted, write_len less than 1024,
  //won't write beyond valid address space, and write_buf is null if write_len is not 0
	if(!mounted){
    return -3;
  }
  if(!has_write_permission){
		return -5;
	}
  if(len > 1024){
    return -2;
  }
  if((addr + len) > (JBOD_DISK_SIZE * JBOD_NUM_DISKS)){
    return -1;
  }
  if(buf == NULL && len != 0){
    return -4;
  }

//get current disk and block where start_addr is located
  int current_disk = getCurrentDisk(addr);
  int current_block = getCurrentBlock(addr);

  //determine number of blocks to write based on write_len
  int num_blocks_to_write = numBlocksCovered(addr, len);
  //determine where in current block start_addr is
  int overflow = (addr % JBOD_DISK_SIZE) % JBOD_BLOCK_SIZE;

  //create buffer to read blocks to, replace with write_bumf's values appropriately, and then write to system
  uint8_t buf1[JBOD_BLOCK_SIZE];

  int have_written = 0;
  int remaining_bytes = len;

  //perform read/write operations num_blocks_to_write times
  for(int i = 0; i < num_blocks_to_write; i++){
    //if past bounds of current disk, move to 0th block of next disk
    if(current_block >= JBOD_NUM_BLOCKS_PER_DISK){
      current_disk++;
      current_block = 0;
    }
    //seek to current disk and block
    jbod_client_operation(buildOperation(current_disk, 0, JBOD_SEEK_TO_DISK), NULL);
    jbod_client_operation(buildOperation(0, current_block, JBOD_SEEK_TO_BLOCK), NULL);
    //read contents from block we are at
    jbod_client_operation(buildOperation(0, 0, JBOD_READ_BLOCK), &buf1[0]);
    int to_write;
    //determine number of bytes to write this iteration (minimum of JBOD_BLOCK_SIZE and remaining bytes to write)
    if(remaining_bytes < JBOD_BLOCK_SIZE){
      to_write = remaining_bytes;
    }
    else{
      to_write = JBOD_BLOCK_SIZE;
    }
    if (i == 0) {
        if (overflow + to_write > JBOD_BLOCK_SIZE) {
            to_write = JBOD_BLOCK_SIZE - overflow;
        }
        memcpy(&buf1[overflow], buf + have_written, to_write);
    } else {
        if (have_written + to_write > len) {
            to_write = len - have_written;
        }
        memcpy(&buf1[0], buf + have_written, to_write);
    }
    //seek to current disk and block
    jbod_client_operation(buildOperation(current_disk, 0, JBOD_SEEK_TO_DISK), NULL);
    jbod_client_operation(buildOperation(0, current_block, JBOD_SEEK_TO_BLOCK), NULL);
    //write block with new values back to disk
    jbod_client_operation(buildOperation(0, 0, JBOD_WRITE_BLOCK), &buf1[0]);
    current_block++;
    have_written += to_write;
    remaining_bytes -= to_write;
    //insert block we just wrote into cache
    cache_insert(current_disk, current_block, &buf1[JBOD_BLOCK_SIZE * i]);
  }

  return len;

}
