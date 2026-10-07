/* Isolated native GPU validation: compiled shaders, drawing, readback,
 * and enough submissions to exercise the eight-bit JM atom-ID wrap. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static GLuint shader(GLenum type, const char *source)
{
   GLuint handle = glCreateShader(type);
   glShaderSource(handle, 1, &source, NULL);
   glCompileShader(handle);
   GLint ok;
   glGetShaderiv(handle, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[4096];
      glGetShaderInfoLog(handle, sizeof(log), NULL, log);
      fprintf(stderr, "Shader compilation failed: %s\n", log);
      exit(1);
   }
   return handle;
}

int main(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   if (!get_display) return 1;
   EGLDisplay display = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint major, minor, count;
   EGLConfig config;
   const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
   const EGLint size[] = {EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};
   const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   if (!eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_ES_API) ||
       !eglChooseConfig(display, attributes, &config, 1, &count) || count != 1) {
      fprintf(stderr, "EGL setup failed: %#x\n", eglGetError());
      return 1;
   }
   EGLSurface surface = eglCreatePbufferSurface(display, config, size);
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
   if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(display, surface, surface, context)) return 1;
   printf("GL_RENDERER=%s\nGL_VERSION=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
   fflush(stdout);
   GLuint vertex = shader(GL_VERTEX_SHADER,
      "attribute vec2 position; void main(){gl_Position=vec4(position,0.0,1.0);}");
   GLuint fragment = shader(GL_FRAGMENT_SHADER,
      "precision mediump float; uniform vec4 colour; void main(){gl_FragColor=colour;}");
   GLuint program = glCreateProgram();
   glAttachShader(program, vertex);
   glAttachShader(program, fragment);
   glBindAttribLocation(program, 0, "position");
   glLinkProgram(program);
   GLint linked;
   glGetProgramiv(program, GL_LINK_STATUS, &linked);
   if (!linked) return 1;
   glUseProgram(program);
   GLint colour = glGetUniformLocation(program, "colour");
   const GLfloat vertices[] = {-1, -1, 3, -1, -1, 3};
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
   glEnableVertexAttribArray(0);
   glViewport(0, 0, 32, 32);
   struct timespec start, end;
   clock_gettime(CLOCK_MONOTONIC, &start);
   for (unsigned frame = 0; frame < 600; ++frame) {
      unsigned expected[4] = {(frame * 17) % 256, (frame * 43) % 256,
                              (frame * 71) % 256, 255};
      glUniform4f(colour, expected[0] / 255.f, expected[1] / 255.f,
                  expected[2] / 255.f, 1.f);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      GLubyte pixel[4] = {0};
      glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
      GLenum error = glGetError();
      for (unsigned component = 0; component < 4; ++component) {
         if (error != GL_NO_ERROR || abs((int)pixel[component] - (int)expected[component]) > 1) {
            fprintf(stderr, "Frame %u failed: error=%#x pixel=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
               frame, error, pixel[0], pixel[1], pixel[2], pixel[3],
               expected[0], expected[1], expected[2], expected[3]);
            return 1;
         }
      }
   }
   clock_gettime(CLOCK_MONOTONIC, &end);
   printf("600 shader draws and pixel checks: PASS (%.3f seconds)\n",
      (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) * 1e-9);
   glDeleteProgram(program);
   glDeleteShader(vertex);
   glDeleteShader(fragment);
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(display, context);
   eglDestroySurface(display, surface);
   eglTerminate(display);
   return 0;
}
