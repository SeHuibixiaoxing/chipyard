#include <stdio.h>
#include <riscv-pk/encoding.h>
#include "marchid.h"
#include <stdint.h>

static inline void tracerv_start_marker(void) {
  asm volatile(".word 0x00008013" ::: "memory");
}

static inline void tracerv_end_marker(void) {
  asm volatile(".word 0x00010013" ::: "memory");
}

int main(void) {
  uint64_t marchid = read_csr(marchid);
  const char* march = get_march(marchid);
  tracerv_start_marker();
  printf("Hello world from core 0, a %s\n", march);
  tracerv_end_marker();
  return 0;
}
