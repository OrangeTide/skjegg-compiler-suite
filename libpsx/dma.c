/* dma.c - libpsx DMA driver (milestone 1).
 *
 * The DMA controller moves blocks between RAM and the peripherals without the
 * CPU copying word by word. libpsx uses channel 2 (GPU) to ship a primitive
 * linked list to the GPU each frame, which is the performance primitive that
 * lets a frame's drawing go out in one transfer rather than through the GP0
 * register FIFO.
 *
 * Channel registers live at 0x1F801080 + n*0x10, reached here through the
 * uncached KSEG1 window (0xBF80xxxx). Channel 2 is the GPU.
 */

#include "psx.h"

/* DMA control register: a 4-bit field per channel, bit 3 of a field enables
 * that channel. Channel 2 (GPU) enable is bit 11; channel 4 (SPU) is bit 19. */
#define DPCR (*(volatile uint32_t *)0xBF8010F0u)
#define DPCR_GPU_ENABLE 0x00000800u
#define DPCR_SPU_ENABLE 0x00080000u

/* GPU channel (2) registers. */
#define D2_MADR (*(volatile uint32_t *)0xBF8010A0u)   /* memory address */
#define D2_BCR  (*(volatile uint32_t *)0xBF8010A4u)   /* block control */
#define D2_CHCR (*(volatile uint32_t *)0xBF8010A8u)   /* channel control */

#define CHCR_BUSY 0x01000000u                         /* transfer in progress */
/* Start a linked-list transfer to the device: busy | linked-list sync mode |
 * direction from RAM to device. */
#define CHCR_LINKED_LIST 0x01000401u

/* GPUSTAT bit 28: the GPU is ready to receive a DMA block. */
#define GPUSTAT (*(volatile uint32_t *)0xBF801814u)
#define GPUSTAT_DMA_READY 0x10000000u

void
psx_dma_init(void)
{
    DPCR = DPCR | DPCR_GPU_ENABLE | DPCR_SPU_ENABLE;
}

void
psx_dma_gpu_list(const uint32_t *head)
{
    while (D2_CHCR & CHCR_BUSY)             /* let any previous transfer finish */
        ;
    while (!(GPUSTAT & GPUSTAT_DMA_READY))  /* and the GPU be ready for DMA */
        ;

    D2_MADR = (uint32_t)(uintptr_t)head & 0x00FFFFFFu;
    D2_BCR = 0;                             /* ignored in linked-list mode */
    D2_CHCR = CHCR_LINKED_LIST;

    while (D2_CHCR & CHCR_BUSY)             /* wait for the list to drain */
        ;
}
