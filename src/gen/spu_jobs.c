/* spu_jobs.c -- SPURS JobQueue job binaries Twisted Metal pushes through
 * cellSpursJq (dumped with SPU_DUMP_MISS, lifted raw at LS 0 with spu_lifter.py
 * --auto-functions over a wrap_spu_elf.py wrapper). Registered by the
 * fingerprint of the raw binary, which is what spu_workload_dispatch_job keys on. */
#include "spu_workload.h"
extern void spu_begin_image(int image_id);
extern void job_0C7C7294454D5F48_spu_func_00000000(spu_context*); extern void job_0C7C7294454D5F48_spu_recomp_register(void);
extern void job_29F5F1C692D71C34_spu_func_00000000(spu_context*); extern void job_29F5F1C692D71C34_spu_recomp_register(void);
extern void job_3641D05246E29D20_spu_func_00000000(spu_context*); extern void job_3641D05246E29D20_spu_recomp_register(void);
extern void job_36DBDE8DC251D8C4_spu_func_00000000(spu_context*); extern void job_36DBDE8DC251D8C4_spu_recomp_register(void);
extern void job_56170BA0CB0F6E31_spu_func_00000000(spu_context*); extern void job_56170BA0CB0F6E31_spu_recomp_register(void);
extern void job_6BA1F6CD43D6005F_spu_func_00000000(spu_context*); extern void job_6BA1F6CD43D6005F_spu_recomp_register(void);
extern void job_6D7C301B7D126EB9_spu_func_00000000(spu_context*); extern void job_6D7C301B7D126EB9_spu_recomp_register(void);
extern void job_8AB0BC8AA6D7FFED_spu_func_00000000(spu_context*); extern void job_8AB0BC8AA6D7FFED_spu_recomp_register(void);
extern void job_A25123E89486EAD5_spu_func_00000000(spu_context*); extern void job_A25123E89486EAD5_spu_recomp_register(void);
extern void job_BD259618FBD16916_spu_func_00000000(spu_context*); extern void job_BD259618FBD16916_spu_recomp_register(void);
extern void job_C06C06F3903D68BB_spu_func_00000000(spu_context*); extern void job_C06C06F3903D68BB_spu_recomp_register(void);
extern void job_CBC884B4E0F08A38_spu_func_00000000(spu_context*); extern void job_CBC884B4E0F08A38_spu_recomp_register(void);
extern void job_D565C5363BCFA78C_spu_func_00000000(spu_context*); extern void job_D565C5363BCFA78C_spu_recomp_register(void);
extern void job_E0CC03D5957DAEAE_spu_func_00000000(spu_context*); extern void job_E0CC03D5957DAEAE_spu_recomp_register(void);
extern void job_FD1877A6E25051E0_spu_func_00000000(spu_context*); extern void job_FD1877A6E25051E0_spu_recomp_register(void);
__attribute__((constructor(200))) static void tm_spu_jobs_register(void)
{
    spu_begin_image(13); job_0C7C7294454D5F48_spu_recomp_register();
    spu_workload_register_img(0x0C7C7294454D5F48ULL, job_0C7C7294454D5F48_spu_func_00000000, 13, "job_0C7C7294454D5F48");
    spu_begin_image(14); job_29F5F1C692D71C34_spu_recomp_register();
    spu_workload_register_img(0x29F5F1C692D71C34ULL, job_29F5F1C692D71C34_spu_func_00000000, 14, "job_29F5F1C692D71C34");
    spu_begin_image(15); job_3641D05246E29D20_spu_recomp_register();
    spu_workload_register_img(0x3641D05246E29D20ULL, job_3641D05246E29D20_spu_func_00000000, 15, "job_3641D05246E29D20");
    spu_begin_image(16); job_36DBDE8DC251D8C4_spu_recomp_register();
    spu_workload_register_img(0x36DBDE8DC251D8C4ULL, job_36DBDE8DC251D8C4_spu_func_00000000, 16, "job_36DBDE8DC251D8C4");
    spu_begin_image(17); job_56170BA0CB0F6E31_spu_recomp_register();
    spu_workload_register_img(0x56170BA0CB0F6E31ULL, job_56170BA0CB0F6E31_spu_func_00000000, 17, "job_56170BA0CB0F6E31");
    spu_begin_image(18); job_6BA1F6CD43D6005F_spu_recomp_register();
    spu_workload_register_img(0x6BA1F6CD43D6005FULL, job_6BA1F6CD43D6005F_spu_func_00000000, 18, "job_6BA1F6CD43D6005F");
    spu_begin_image(19); job_6D7C301B7D126EB9_spu_recomp_register();
    spu_workload_register_img(0x6D7C301B7D126EB9ULL, job_6D7C301B7D126EB9_spu_func_00000000, 19, "job_6D7C301B7D126EB9");
    spu_begin_image(20); job_8AB0BC8AA6D7FFED_spu_recomp_register();
    spu_workload_register_img(0x8AB0BC8AA6D7FFEDULL, job_8AB0BC8AA6D7FFED_spu_func_00000000, 20, "job_8AB0BC8AA6D7FFED");
    spu_begin_image(21); job_A25123E89486EAD5_spu_recomp_register();
    spu_workload_register_img(0xA25123E89486EAD5ULL, job_A25123E89486EAD5_spu_func_00000000, 21, "job_A25123E89486EAD5");
    spu_begin_image(22); job_BD259618FBD16916_spu_recomp_register();
    spu_workload_register_img(0xBD259618FBD16916ULL, job_BD259618FBD16916_spu_func_00000000, 22, "job_BD259618FBD16916");
    spu_begin_image(23); job_C06C06F3903D68BB_spu_recomp_register();
    spu_workload_register_img(0xC06C06F3903D68BBULL, job_C06C06F3903D68BB_spu_func_00000000, 23, "job_C06C06F3903D68BB");
    spu_begin_image(24); job_CBC884B4E0F08A38_spu_recomp_register();
    spu_workload_register_img(0xCBC884B4E0F08A38ULL, job_CBC884B4E0F08A38_spu_func_00000000, 24, "job_CBC884B4E0F08A38");
    spu_begin_image(25); job_D565C5363BCFA78C_spu_recomp_register();
    spu_workload_register_img(0xD565C5363BCFA78CULL, job_D565C5363BCFA78C_spu_func_00000000, 25, "job_D565C5363BCFA78C");
    spu_begin_image(26); job_E0CC03D5957DAEAE_spu_recomp_register();
    spu_workload_register_img(0xE0CC03D5957DAEAEULL, job_E0CC03D5957DAEAE_spu_func_00000000, 26, "job_E0CC03D5957DAEAE");
    spu_begin_image(27); job_FD1877A6E25051E0_spu_recomp_register();
    spu_workload_register_img(0xFD1877A6E25051E0ULL, job_FD1877A6E25051E0_spu_func_00000000, 27, "job_FD1877A6E25051E0");
    spu_begin_image(0);
}
