/* Off-screen vendor EGL/GLES validation. Opens card0 without becoming DRM
 * master, creates a surfaceless context, and checks pixels in a private FBO.
 * Build against the ordinary EGL/GLES/GBM ABI; select trial libraries only
 * in this process's environment. No compositor or boot settings are changed. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static GLuint compile(GLenum type, const char *source)
{
   GLuint object = glCreateShader(type);
   glShaderSource(object, 1, &source, NULL);
   glCompileShader(object);
   GLint ok = 0;
   glGetShaderiv(object, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[4096];
      glGetShaderInfoLog(object, sizeof(log), NULL, log);
      fprintf(stderr, "COMPILE_FAIL: %s\n", log);
      exit(1);
   }
   return object;
}

static void check_pixel(unsigned frame, const unsigned expected[4])
{
   GLubyte pixel[4] = {0};
   glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
   GLenum error = glGetError();
   for (unsigned component = 0; component < 4; ++component) {
      if (error != GL_NO_ERROR ||
          abs((int)pixel[component] - (int)expected[component]) > 1) {
         fprintf(stderr, "PIXEL_FAIL frame=%u error=%#x pixel=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                 frame, error, pixel[0], pixel[1], pixel[2], pixel[3],
                 expected[0], expected[1], expected[2], expected[3]);
         exit(1);
      }
   }
}

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   char *parse_end = NULL;
   unsigned long requested = argc > 1 ? strtoul(argv[1], &parse_end, 10) : 1;
   if (!requested || requested > 2000 ||
       (parse_end && (*parse_end || parse_end == argv[1]))) return 2;
   unsigned frames = requested;
   int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
   if (fd < 0) { perror("card0"); return 1; }
   struct gbm_device *device = gbm_create_device(fd);
   if (!device) { fprintf(stderr, "GBM_FAIL\n"); return 1; }
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   if (!get_display) return 1;
   EGLDisplay display = get_display(EGL_PLATFORM_GBM_KHR, device, NULL);
   EGLint major, minor;
   if (!eglInitialize(display, &major, &minor)) {
      fprintf(stderr, "INIT_FAIL error=%#x\n", eglGetError()); return 1;
   }
   printf("EGL=%d.%d VENDOR=%s\n", major, minor, eglQueryString(display, EGL_VENDOR));
   const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
   if (!extensions || !strstr(extensions, "EGL_KHR_surfaceless_context")) {
      fprintf(stderr, "SURFACELESS_UNSUPPORTED\n"); return 1;
   }
   EGLConfig config;
   EGLint count;
   const EGLint attributes[] = {EGL_SURFACE_TYPE, 0,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
   const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   if (!eglBindAPI(EGL_OPENGL_ES_API) ||
       !eglChooseConfig(display, attributes, &config, 1, &count) || count != 1) {
      fprintf(stderr, "CONFIG_FAIL error=%#x\n", eglGetError()); return 1;
   }
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
   if (context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
      fprintf(stderr, "CONTEXT_FAIL error=%#x\n", eglGetError()); return 1;
   }
   printf("GL_RENDERER=%s\nGL_VERSION=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
   GLuint texture, fbo;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "FBO_FAIL\n"); return 1;
   }
   glViewport(0, 0, 32, 32);
   glClearColor(32.f / 255, 64.f / 255, 128.f / 255, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   const unsigned clear_expected[] = {32, 64, 128, 255};
   check_pixel(0, clear_expected);
   puts("CLEAR_READBACK_PASS");
   GLuint vertex = compile(GL_VERTEX_SHADER,
      "attribute vec2 position; void main(){gl_Position=vec4(position,0.0,1.0);}");
   GLuint fragment = compile(GL_FRAGMENT_SHADER,
      "precision mediump float; uniform vec4 colour; void main(){gl_FragColor=colour;}");
   GLuint program = glCreateProgram();
   glAttachShader(program, vertex);
   glAttachShader(program, fragment);
   glBindAttribLocation(program, 0, "position");
   glLinkProgram(program);
   GLint linked;
   glGetProgramiv(program, GL_LINK_STATUS, &linked);
   if (!linked) { fprintf(stderr, "LINK_FAIL\n"); return 1; }
   puts("SHADER_LINK_PASS");
   glUseProgram(program);
   GLint colour = glGetUniformLocation(program, "colour");
   const GLfloat vertices[] = {-1, -1, 3, -1, -1, 3};
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
   glEnableVertexAttribArray(0);
   struct timespec start, end;
   clock_gettime(CLOCK_MONOTONIC, &start);
   for (unsigned frame = 0; frame < frames; ++frame) {
      unsigned expected[4] = {(frame * 17) % 256, (frame * 43) % 256,
                              (frame * 71) % 256, 255};
      glUniform4f(colour, expected[0] / 255.f, expected[1] / 255.f,
                  expected[2] / 255.f, 1.f);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      check_pixel(frame + 1, expected);
   }
   clock_gettime(CLOCK_MONOTONIC, &end);
   printf("SHADER_READBACK_PASS frames=%u seconds=%.6f\n", frames,
      (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) * 1e-9);
   glDeleteProgram(program);
   glDeleteShader(vertex);
   glDeleteShader(fragment);
   glDeleteFramebuffers(1, &fbo);
   glDeleteTextures(1, &texture);
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(display, context);
   eglTerminate(display);
   gbm_device_destroy(device);
   close(fd);
   puts("CLEANUP_PASS");
   return 0;
}
