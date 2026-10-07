/* Diagnostic preload for the isolated EGL test only. Logs request numbers
 * and outcomes, never ioctl payloads or typed input. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

int ioctl(int fd, unsigned long request, ...)
{
   static int (*real_ioctl)(int, unsigned long, ...);
   if (!real_ioctl)
      real_ioctl = dlsym(RTLD_NEXT, "ioctl");
   va_list args;
   va_start(args, request);
   void *argument = va_arg(args, void *);
   va_end(args);
   /* Optional, bounded experiment on our one-atom JM diagnostic only. */
   if (request == 0x40108002 && getenv("VEGAS_JM_COHERENT")) {
      struct { uint64_t address; uint32_t count, stride; } submission;
      memcpy(&submission, argument, sizeof(submission));
      if (submission.count == 1 && submission.stride == 48) {
         uint32_t *requirements = (uint32_t *)(uintptr_t)(submission.address + 44);
         *requirements |= 1u << 6;
      }
   }
   int result = real_ioctl(fd, request, argument);
   int saved_errno = errno;
   fprintf(stderr, "[vegas-ioctl] fd=%d request=%#lx result=%d errno=%d\n",
           fd, request, result, result < 0 ? saved_errno : 0);
   errno = saved_errno;
   return result;
}
