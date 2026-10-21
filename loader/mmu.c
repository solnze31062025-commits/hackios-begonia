/*
 * Bootstrap page tables for the stage-2 loader.
 *
 * XNU's start_first_cpu assumes MMU is already on, in the "iBoot style":
 *   - a KVA block mapping (kernel VMAs -> load address)
 *   - XNU then builds its own bootstrap tables; ours only has to bridge
 *     the jump from the loader to the kernel entry point.
 *
 * Layout: 4K granule, 48-bit VA (T0SZ/T1SZ = 16), 44-bit PA (IPS=3).
 *
 *   PGD (L0, 1 per 512GB):  PGD[  0] -> dram tables   (0x40000000 DRAM)
 *                           PGD[511] -> kva tables    (0xFFFFFE...) kernel link
 *   PUD (L1, 1GB per entry):
 *     dram: [1] -> pmd_dram1 (0x40000000..0x7FFFFFFF), [2] -> pmd_dram2
 *     kva : [(VA>>30)&0x1FF] -> pmd_kva (1GB: XNU_KVA_BLOCK -> 0x40000000)
 *   PMD (L2, 2MB blocks)
 */
#include "loader.h"

#define TTE_TABLE        (3ULL)                  /* bits[1:0] = 01 + table */
#define TTE_BLOCK        (1ULL)                  /* bits[1:0] = 01 */
#define TTE_AF           (1ULL << 10)            /* access flag */
#define TTE_SH_INNER     (3ULL << 8)             /* inner shareable */
#define TTE_NORMAL(OpIdx) (((uint64_t)(OpIdx)) << 2)

static uint64_t pgd[512]        __attribute__((aligned(4096)));
static uint64_t pud_dram[512]   __attribute__((aligned(4096)));
static uint64_t pmd_dram1[512]  __attribute__((aligned(4096)));
static uint64_t pmd_dram2[512]  __attribute__((aligned(4096)));
static uint64_t pud_kva[512]    __attribute__((aligned(4096)));
static uint64_t pmd_kva[512]    __attribute__((aligned(4096)));

static void
map_2mb_blocks(uint64_t *table, uint64_t va_base, uint64_t pa_base, uint64_t size)
{
	uint64_t descr = TTE_BLOCK | TTE_AF | TTE_SH_INNER | TTE_NORMAL(0);
	for (uint64_t off = 0; off < size; off += 0x200000ULL) {
		uint64_t idx = (((va_base + off) >> 21) & 0x1FF);
		table[idx] = (pa_base + off) | descr;
	}
}

void
mmu_boot_enable(void)
{
	uint64_t dram_pud1 = (0x40000000ULL >> 30) & 0x1FF; /* = 1 */
	uint64_t dram_pud2 = (0x80000000ULL >> 30) & 0x1FF; /* = 2 */
	uint64_t kva_pud   = (XNU_KVA_BLOCK >> 30) & 0x1FF;

	pgd[0] = ((uint64_t)pud_dram) | TTE_TABLE;
	pgd[511] = ((uint64_t)pud_kva) | TTE_TABLE;   /* VA bits[47:39] == 111111111 */

	pud_dram[dram_pud1] = ((uint64_t)pmd_dram1) | TTE_TABLE;
	pud_dram[dram_pud2] = ((uint64_t)pmd_dram2) | TTE_TABLE;
	map_2mb_blocks(pmd_dram1, 0x40000000ULL, 0x40000000ULL, 0x40000000ULL);
	map_2mb_blocks(pmd_dram2, 0x80000000ULL, 0x80000000ULL, 0x40000000ULL);

	pud_kva[kva_pud] = ((uint64_t)pmd_kva) | TTE_TABLE;
	map_2mb_blocks(pmd_kva, XNU_KVA_BLOCK, 0x40000000ULL, 0x40000000ULL);

	uint64_t ttbr = (uint64_t)pgd;
	uint64_t tcr  = (16ULL << 0)   /* T0SZ = 16 → 48-bit VA */
	    | (0x55ULL << 8)           /* IRGN0=01, ORGN0=01: WB table walks */
	    | (3ULL << 12)             /* SH0: inner-shareable */
	    | (3ULL << 16)             /* IPS = 3 → 42-bit PA (enough for 4GB+) */
	    | (16ULL << 32)            /* T1SZ = 16 */
	    | (0x55ULL << 38)          /* IRGN1/ORGN1: WB walks */
	    | (3ULL << 42)             /* SH1: inner-shareable */
	    | (2ULL << 46);            /* TG1 = 10 → 4K (TG1 encoding differs) */
	(void)kva_pud;

	__asm__ volatile(
	    "msr TTBR0_EL1, %0\n"
	    "msr TTBR1_EL1, %0\n"
	    "msr TCR_EL1, %1\n"
	    "isb sy\n"
	    :: "r"(ttbr), "r"(tcr) : "memory");

	/* ATTR0 = Normal (0x44), the only attribute we use. */
	uint64_t mair = 0x0000000000000044ULL;
	__asm__ volatile("msr MAIR_EL1, %0\n" :: "r"(mair));

	uint64_t sctlr;
	__asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(sctlr));
	sctlr |= (1ULL << 0);  /* M */
	sctlr |= (1ULL << 2);  /* C */
	sctlr |= (1ULL << 12); /* I */
	__asm__ volatile("isb sy\n");
	__asm__ volatile("msr SCTLR_EL1, %0\nisb sy\n" :: "r"(sctlr) : "memory");
}
