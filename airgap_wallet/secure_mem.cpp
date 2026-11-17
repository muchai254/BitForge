#include "secure_mem.h"
#include <alloca.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Left untouched at the bottom of the stack: FreeRTOS's overflow canary lives
// there, plus room for Xtensa register-window spills.
#define SCRUB_BOTTOM_MARGIN 768
#define STACK_FILL_BYTE 0xA5 // tskSTACK_FILL_BYTE

static portMUX_TYPE scrubMux = portMUX_INITIALIZER_UNLOCKED;

// Fills from just below its own frame down to the margin. It must make NO
// function calls after the alloca: on Xtensa a call made while the stack
// pointer is this low puts its frame and register spills below the fill,
// inside the margin. The high-water mark then reads "almost full".
__attribute__((noinline)) static size_t fillUnusedStack(uint8_t *bottom) {
  uint8_t here;
  uint8_t *sp = &here;
  if (sp <= bottom + SCRUB_BOTTOM_MARGIN + 64) return 0;
  size_t n = (size_t)(sp - bottom) - SCRUB_BOTTOM_MARGIN - 64;
  volatile uint8_t *p = (volatile uint8_t *)alloca(n);
  for (size_t i = 0; i < n; i++) p[i] = STACK_FILL_BYTE;
  return n;
}

// Not inlined, so all of this sits below the caller's frame.
__attribute__((noinline)) size_t secure_scrub_stack() {
  uint8_t *bottom = pxTaskGetStackStart(NULL); // lowest address; the stack grows down
  if (bottom == nullptr) return 0;
  // Interrupts off on this core while filling (~0.25 ms for 15 KB): an
  // interrupt would save its context on this task's stack, below the fill.
  // Entered and left here, at a normal stack depth, not inside the fill.
  portENTER_CRITICAL(&scrubMux);
  size_t n = fillUnusedStack(bottom);
  portEXIT_CRITICAL(&scrubMux);
  return n;
}

#define HEAP_SCRUB_MAX_BLOCKS 48
#define HEAP_SCRUB_MIN_BLOCK 64

size_t secure_scrub_free_heap() {
  void *blocks[HEAP_SCRUB_MAX_BLOCKS];
  int n = 0;
  size_t total = 0;
  // Largest block first, until only crumbs are left.
  while (n < HEAP_SCRUB_MAX_BLOCKS) {
    size_t sz = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (sz < HEAP_SCRUB_MIN_BLOCK) break;
    void *p = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
    if (!p) break;
    secure_zero(p, sz);
    blocks[n++] = p;
    total += sz;
  }
  for (int i = 0; i < n; i++) heap_caps_free(blocks[i]);
  return total;
}
