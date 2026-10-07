/* EGL_ANDROID_native_fence_sync on the kbase JM driver, as Chrome's GPU
 * process uses it: render, create a native fence sync, flush, dup its fd,
 * check the sync_file signals, then import the fd and wait on it both ways.
 * Many iterations cross the eight-bit JM atom-ID wrap.
 *
 * usage: gpu-fence-probe ITERATIONS
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
   int iterations = argc > 1 ? atoi(argv[1]) : 300;
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay d = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint maj, min;
   if (!eglInitialize(d, &maj, &min) || !eglBindAPI(EGL_OPENGL_ES_API)) return 1;
   const char *exts = eglQueryString(d, EGL_EXTENSIONS);
   printf("native_fence_sync advertised: %s\n",
          exts && strstr(exts, "EGL_ANDROID_native_fence_sync") ? "yes" : "NO");
   const EGLint ver[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   EGLContext ctx = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ver);
   if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return 1;

   PFNEGLCREATESYNCKHRPROC create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
   PFNEGLDESTROYSYNCKHRPROC destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
   PFNEGLCLIENTWAITSYNCKHRPROC client_wait = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
   PFNEGLWAITSYNCKHRPROC server_wait = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
   PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fd =
      (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
   if (!create_sync || !dup_fd || !client_wait || !server_wait) {
      fprintf(stderr, "missing sync entry points\n");
      return 1;
   }

   GLuint tex, fb;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1080, 2388, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   glViewport(0, 0, 1080, 2388);

   int fails = 0;
   double signal_ms = 0;
   for (int i = 0; i < iterations && fails < 5; ++i) {
      /* several batches per iteration so syncobjs carry many atoms */
      for (int b = 0; b < 1 + i % 4; ++b) {
         glClearColor((i % 7) / 7.f, (b % 3) / 3.f, 0.5f, 1);
         glClear(GL_COLOR_BUFFER_BIT);
         glFlush();
      }
      EGLSyncKHR sync = create_sync(d, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
      if (sync == EGL_NO_SYNC_KHR) { fprintf(stderr, "iter %d: create sync failed %#x\n", i, eglGetError()); ++fails; continue; }
      glFlush();
      int fd = dup_fd(d, sync);
      if (fd < 0) { fprintf(stderr, "iter %d: eglDupNativeFenceFDANDROID = %d (%#x)\n", i, fd, eglGetError()); ++fails; destroy_sync(d, sync); continue; }
      double t0 = now_ms();
      struct pollfd p = {.fd = fd, .events = POLLIN};
      int r = poll(&p, 1, 2000);
      signal_ms += now_ms() - t0;
      if (r != 1) { fprintf(stderr, "iter %d: fence fd %d never signaled (poll=%d)\n", i, fd, r); ++fails; }

      /* import the fd back (as a compositor would) and wait on it */
      const EGLint attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, dup(fd), EGL_NONE};
      EGLSyncKHR imported = create_sync(d, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
      if (imported == EGL_NO_SYNC_KHR) { fprintf(stderr, "iter %d: import failed %#x\n", i, eglGetError()); ++fails; }
      else {
         if (!server_wait(d, imported, 0)) { fprintf(stderr, "iter %d: eglWaitSyncKHR failed\n", i); ++fails; }
         if (client_wait(d, imported, 0, 1000000000ull) != EGL_CONDITION_SATISFIED_KHR) {
            fprintf(stderr, "iter %d: eglClientWaitSyncKHR not satisfied\n", i); ++fails;
         }
         destroy_sync(d, imported);
      }
      close(fd);
      destroy_sync(d, sync);
   }
   printf("%s: %d iterations, avg fence signal wait %.2f ms\n",
          fails ? "FAIL" : "PASS", iterations, signal_ms / iterations);
   return fails ? 1 : 0;
}
