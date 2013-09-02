#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

int main() {
  int xfd = open("/dev/cmos/0", O_RDONLY);
  if(xfd < 0) {
    perror("open cmos");
    goto error;
  }
  if(lseek(xfd, 0x09 * 8, SEEK_SET) < 0) {
    perror("lseek year");
    goto error;
  }
  uint8_t year;
  if(read(xfd, &year, 8) < 0) {
    perror("lseek year");
    goto error;
  }

  printf("year is %x\n", year);
  return 0;

error:
  close(xfd);
  return -1;
}
