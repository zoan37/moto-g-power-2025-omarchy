/* Can the Mali (kbase Panfrost) render straight into MediaTek KMS scanout
 * buffers? Mirrors Hyprland's direct-DRM path: GBM scanout BO on card0,
 * exported as a DMA-BUF, imported into a surfaceless EGL context as an
 * EGLImage render target, drawn, finished, then verified through a CPU map
 * of the same BO (what the display controller will scan out).
 *
 * usage: gpu-scanout-probe FRAMES
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define W 1080
#define H 2388

int main(int argc, char **argv)
{
   int frames = argc > 1 ? atoi(argv[1]) : 60;
   int card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
   struct gbm_device *gbm = gbm_create_device(card);
   struct gbm_bo *bo = gbm ? gbm_bo_create(gbm, W, H, DRM_FORMAT_XRGB8888,
                                         GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING) : NULL;
   if (!bo) { fprintf(stderr, "GBM scanout BO failed\n"); return 1; }
   int fd = gbm_bo_get_fd(bo);
   unsigned stride = gbm_bo_get_stride(bo);

   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay d = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint maj, min;
   const EGLint ver[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   if (!eglInitialize(d, &maj, &min) || !eglBindAPI(EGL_OPENGL_ES_API)) return 1;
   EGLContext ctx = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ver);
   if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return 1;
   printf("GL_RENDERER=%s stride=%u\n", glGetString(GL_RENDERER), stride);

   const EGLint attrs[] = {
      EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
      EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride, EGL_NONE};
   PFNEGLCREATEIMAGEKHRPROC create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
   PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC rb_storage =
      (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
   EGLImageKHR img = create_image(d, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
   if (img == EGL_NO_IMAGE_KHR) { fprintf(stderr, "dmabuf import failed %#x\n", eglGetError()); return 1; }
   GLuint rb, fb;
   glGenRenderbuffers(1, &rb);
   glBindRenderbuffer(GL_RENDERBUFFER, rb);
   rb_storage(GL_RENDERBUFFER, img);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "scanout FBO incomplete\n");
      return 1;
   }

   int fails = 0;
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (int f = 0; f < frames && fails < 5; ++f) {
      unsigned r = (f * 37) & 0xff, g = (f * 91) & 0xff, b = (f * 13) & 0xff;
      glViewport(0, 0, W, H);
      glDisable(GL_SCISSOR_TEST);
      glClearColor(r / 255.f, g / 255.f, b / 255.f, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      /* a damage-style partial update, like a typed character */
      glEnable(GL_SCISSOR_TEST);
      glScissor(100 + f % 50, 200, 40, 60);
      glClearColor(1, 1, 1, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glFinish();
      /* check through the BO's own CPU mapping (what KMS scans out) */
      uint32_t map_stride;
      void *map_data = NULL;
      unsigned char *px = gbm_bo_map(bo, 0, 0, W, H, GBM_BO_TRANSFER_READ, &map_stride, &map_data);
      if (!px) { fprintf(stderr, "gbm_bo_map failed\n"); return 1; }
      /* FBOs on imported images are not flipped: GL y=200..259 is BO rows 200..259 */
      unsigned char *bg = px + (size_t)10 * map_stride + 10 * 4;
      unsigned char *wh = px + (size_t)230 * map_stride + (120 + f % 50) * 4;
      if (abs(bg[2] - (int)r) > 1 || abs(bg[1] - (int)g) > 1 || abs(bg[0] - (int)b) > 1 ||
          wh[0] != 255 || wh[1] != 255 || wh[2] != 255) {
         fprintf(stderr, "frame %d: bg %u,%u,%u want %u,%u,%u; patch %u,%u,%u want 255\n", f,
                 bg[2], bg[1], bg[0], r, g, b, wh[2], wh[1], wh[0]);
         ++fails;
      }
      gbm_bo_unmap(bo, map_data);
   }
   clock_gettime(CLOCK_MONOTONIC, &t1);
   double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
   printf("%s: %d frames into the scanout BO, %.2f ms/frame (incl. glFinish + CPU check)\n",
          fails ? "FAIL" : "PASS", frames, s * 1000 / frames);
   return fails ? 1 : 0;
}
