#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <err.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include "net.h"
#include "jbod.h"

/* the client socket descriptor for the connection to the server */
int cli_sd = -1;

/* attempts to read n bytes from fd; returns true on success and false on
 * failure */
/*bool nread(int fd, int len, uint8_t *buf) {
  if(fd == -1){
    return false;
  }

  //read len number of bytes from fd into buf
  int bytes_read = read(fd, buf, len);

  if(bytes_read != len){
    return false;
  }
  return true;
}*/
bool nread(int fd, int len, uint8_t *buf) {
  if(fd == -1){
    return false;
  }
  
  int bytes_read = 0;
  int res;

  while(bytes_read < len){
    res = read(fd, &buf[bytes_read], len - bytes_read);
    if(res == -1){
      return false;
    }
    bytes_read += res;
  }
  return true;
}

/* attempts to write n bytes to fd; returns true on success and false on
 * failure */
/*bool nwrite(int fd, int len, uint8_t *buf) {
  if(fd == -1){
    return false;
  }

  //write len number of bytes of buf to fd
  int bytes_written = write(fd, buf, len);

  if(bytes_written != len){
    return false;
  }
  return true;

}*/

bool nwrite(int fd, int len, uint8_t *buf) {
  if(fd == -1){
    return false;
  }

  int bytes_written = 0;
  int res;

  while(bytes_written < len){
    res = write(fd, &buf[bytes_written], len - bytes_written);
    if(res == -1){
      return false;
    }
    bytes_written += res;
  }
  return true;
}

/* attempts to receive a packet from fd; returns true on success and false on
 * failure */
bool recv_packet(int fd, uint32_t *op, uint8_t *ret, uint8_t *block) {
  //I AM PRETTY SURE OP DOESN'T MATTER WHEN RECEIVING A PACKET. ONLY RET AND BLOCK DO. PROBABLY DON'T EVEN NEED TO BE WORRIED ABOUT OP IN THIS FUNCTION

  //create buffer to read header of packet into
  uint8_t buf[HEADER_LEN];
  if(!nread(fd, HEADER_LEN, buf)){
    return false;
  }
  //successfully read packet header, need to convert it back into op and status format
  //*op = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | (buf[3]);
  memcpy(op, buf, 4);
  //convert op from network byte order to host byte order
  *op = ntohl(*op);
  *ret = buf[4];

  if(block == NULL){
    return true;
  }
  //check if second to last bit of ret is one, if it is, then payload/block exists and need to read JBOD_BLOCK_SIZE more bytes
  if(*ret && 2){
    if(!nread(fd, JBOD_BLOCK_SIZE, block)){
      return false;
    }
  }

  return true;
}

/* attempts to send a packet to sd; returns true on success and false on
 * failure */
bool send_packet(int fd, uint32_t op, uint8_t *block) {
  //create buf for the packet we are going to send
  uint8_t buf[HEADER_LEN + JBOD_BLOCK_SIZE] = {0};
  //len of the packet we are going to send (if no payload, it is only the header)
  int len = HEADER_LEN;

  //convert op from host byte ordering to network byte ordering
  op = htonl(op);
  //pack op into buf, splitting it into 4 bytes
  //buf[0] = (op >> 24) & 0xff;
  //buf[1] = (op >> 16) & 0xff;
  //buf[2] = (op >> 8) & 0xff;
  //buf[3] = op & 0xff;
  //copy OP into buf
  memcpy(buf, &op, 4);

  //if there is a payload, adjust length accordingly, copy block into buf,
  //and set the info code (second to last bit of 5th byte of buf) to 1
  if(block != NULL){
    len += JBOD_BLOCK_SIZE;
    memcpy(&buf[HEADER_LEN], block, JBOD_BLOCK_SIZE);
    buf[4] = buf[4] | 2;
  }

  //write buf
  if(!nwrite(fd, len, buf)){
    return false;
  }
  return true;
  
}

/* connect to server and set the global client variable to the socket */
bool jbod_connect(const char *ip, uint16_t port) {
  if(cli_sd != -1){
    return false;
  }
  struct sockaddr_in caddr;
  //set to AF_INET for IPv4
  caddr.sin_family = AF_INET;
  caddr.sin_port = htons(port);
  //convert passed IP address to UNIX structure, return false if fails
  if(inet_aton(ip, &caddr.sin_addr) == 0){
    return false;
  }

  //create socket, return false if fails
  cli_sd = socket(AF_INET, SOCK_STREAM, 0);
  if(cli_sd == 0){
    return false;
  }
  
  //connect to socket, return false if fails
  if(connect(cli_sd, (const struct sockaddr *)&caddr, sizeof(caddr)) == -1){
    printf("\nConnecting to socket failed.\n");
    return false;
  }

  return true;

}

//to disconnect, close cli_sd and set cli_sd to -1
void jbod_disconnect(void) {
  close(cli_sd);
  cli_sd = -1;
  return;
}

int jbod_client_operation(uint32_t op, uint8_t *block) {
  if(cli_sd == -1){
    return -1;
  }
  if(!send_packet(cli_sd, op, block)){
    return -1;
  }

  uint8_t ret;
  if(!recv_packet(cli_sd, &op, &ret, block)){
    return -1;
  }

  //last bit of ret contains value returned by jbod_operation call
  //if last bit of ret is not 0, then jbod_operation returned -1 (failure).
  if(!(ret && 1)){
    return -1;
  }

  return 1;
}
