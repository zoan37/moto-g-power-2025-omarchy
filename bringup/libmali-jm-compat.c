/* Process-local adapter for the MT6835 stock JM kernel and the isolated r48
 * libmali trial. This is not a general Mali compatibility layer.
 *
 * The vendor kernel selects atom V2 for strides 48/64 and V3 for 56/72.
 * This upstream r48 library submits 64-byte V3 atoms (seq_nr at byte 0,
 * jc at 8, atom_number at 48 and core_req at 52). Copy each to a zero-tailed
 * 72-byte V3 record so the kernel interprets the preserved fields correctly.
 * The additional vendor flush-ID field at byte 64 stays zero.
 *
 * Enable only for the trial process with VEGAS_LIBMALI_JM_COMPAT=stride72.
 * Never preload into the current Mesa compositor or install system-wide.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

static int (*original_ioctl)(int, unsigned long, ...);
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void resolve_ioctl(void)
{
   original_ioctl = dlsym(RTLD_NEXT, "ioctl");
}

int ioctl(int fd, unsigned long request, ...)
{
   pthread_once(&once, resolve_ioctl);
   if (!original_ioctl) { errno = ENOSYS; return -1; }
   va_list args;
   va_start(args, request);
   void *argument = va_arg(args, void *);
   va_end(args);
   const char *mode = getenv("VEGAS_LIBMALI_JM_COMPAT");
   if (request != 0x40108002 || !mode || strcmp(mode, "stride72"))
      return original_ioctl(fd, request, argument);

   struct stat device, opened;
   if (stat("/dev/mali0", &device) || fstat(fd, &opened) ||
       !S_ISCHR(device.st_mode) || !S_ISCHR(opened.st_mode) ||
       device.st_rdev != opened.st_rdev) {
      errno = ENODEV; return -1;
   }
   struct { uint64_t address; uint32_t count, stride; } submission;
   if (!argument) { errno = EINVAL; return -1; }
   memcpy(&submission, argument, sizeof(submission));
   if (submission.stride != 64 || !submission.count ||
       submission.count > 256 || !submission.address) {
      errno = EINVAL; return -1;
   }
   uint64_t records[256 * 9];
   unsigned char *bytes = (unsigned char *)records;
   for (unsigned i = 0; i < submission.count; ++i) {
      const unsigned char *source =
         (const unsigned char *)(uintptr_t)submission.address + i * 64;
      for (unsigned j = 57; j < 64; ++j) {
         if (source[j]) { errno = EINVAL; return -1; }
      }
      memcpy(bytes + i * 72, source, 64);
      memset(bytes + i * 72 + 64, 0, 8);
   }
   submission.address = (uintptr_t)records;
   submission.stride = 72;
   int result = original_ioctl(fd, request, &submission);
   int saved_errno = errno;
   if (getenv("VEGAS_LIBMALI_TRACE"))
      fprintf(stderr, "[libmali-compat] atoms=%u stride=72 result=%d errno=%d\n",
              submission.count, result, result < 0 ? saved_errno : 0);
   errno = saved_errno;
   return result;
}
