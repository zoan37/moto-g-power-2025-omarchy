/* Off-screen reproduction of Hyprland-style GLES work on the native Mali
 * trial driver: a work FBO (optionally with depth/stencil, as Hyprland's
 * work buffers have), BGRA texture uploads, blended textured quads under
 * several scissor rects, then a copy pass into a second "output" FBO.
 * Every frame checks pixels; kbase faults are printed by Mesa on stderr.
 *
 * usage: gpu-hypr-probe W H FRAMES [flags]
 *   flags: s = depth/stencil on work FBO, b = blending, c = scissor rects,
 *          t = texture upload each frame, o = copy pass to output FBO,
 *          x = draw into a small FBO with a scissor entirely outside it
 *              (Hyprland's monitor-space damage rects vs. small FBOs)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static GLuint program(const char *vs, const char *fs)
{
   GLuint p = glCreateProgram();
   glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
   glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation(p, 0, "pos");
   glLinkProgram(p);
   GLint ok;
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) exit(1);
   return p;
}

static GLuint target(int w, int h, int depth_stencil, GLuint *tex_out)
{
   GLuint tex, fb;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   if (depth_stencil) {
      GLuint rb;
      glGenRenderbuffers(1, &rb);
      glBindRenderbuffer(GL_RENDERBUFFER, rb);
      glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, w, h);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
   }
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "FBO %dx%d incomplete\n", w, h);
      exit(1);
   }
   *tex_out = tex;
   return fb;
}

static void quad(float x0, float y0, float x1, float y1)
{
   const GLfloat v[] = {x0, y0, x1, y0, x0, y1, x1, y1};
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
   glEnableVertexAttribArray(0);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static int check(int x, int y, const unsigned char want[4], int frame, const char *what)
{
   unsigned char px[4] = {0};
   glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   for (int i = 0; i < 4; ++i)
      if (abs(px[i] - want[i]) > 2) {
         fprintf(stderr, "frame %d %s (%d,%d): got %u,%u,%u,%u want %u,%u,%u,%u\n", frame, what, x, y,
                 px[0], px[1], px[2], px[3], want[0], want[1], want[2], want[3]);
         return 1;
      }
   return 0;
}

int main(int argc, char **argv)
{
   if (argc < 4) {
      fprintf(stderr, "usage: %s W H FRAMES [sbcto]\n", argv[0]);
      return 2;
   }
   int W = atoi(argv[1]), H = atoi(argv[2]), frames = atoi(argv[3]);
   const char *f = argc > 4 ? argv[4] : "";
   int ds = !!strchr(f, 's'), blend = !!strchr(f, 'b'), scis = !!strchr(f, 'c');
   int upload = !!strchr(f, 't'), copy = !!strchr(f, 'o'), outside = !!strchr(f, 'x');

   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay d = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint maj, min;
   const EGLint ver[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   if (!eglInitialize(d, &maj, &min) || !eglBindAPI(EGL_OPENGL_ES_API)) return 1;
   EGLContext ctx = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ver);
   if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
      fprintf(stderr, "context failed %#x\n", eglGetError());
      return 1;
   }
   printf("GL_RENDERER=%s %dx%d frames=%d flags=%s\n", glGetString(GL_RENDERER), W, H, frames, f);
   fflush(stdout);

   GLuint solid = program("attribute vec2 pos; void main(){gl_Position=vec4(pos,0.0,1.0);}",
      "precision mediump float; uniform vec4 col; void main(){gl_FragColor=col;}");
   GLuint texp = program("attribute vec2 pos; varying vec2 uv;"
      "void main(){uv=pos*0.5+0.5; gl_Position=vec4(pos,0.0,1.0);}",
      "precision mediump float; varying vec2 uv; uniform sampler2D tex; uniform float alpha;"
      "void main(){gl_FragColor=texture2D(tex,uv)*alpha;}");

   GLuint work_tex, out_tex, src_tex;
   GLuint work = target(W, H, ds, &work_tex);
   GLuint out = copy ? target(W, H, 0, &out_tex) : 0;
   GLuint small_tex, small = outside ? target(64, 64, 0, &small_tex) : 0;

   /* A client-like BGRA texture (Hyprland uploads SHM as BGRA_EXT). */
   int TW = W * 2 / 3, TH = H * 2 / 3;
   unsigned char *pix = malloc((size_t)TW * TH * 4);
   glGenTextures(1, &src_tex);
   glBindTexture(GL_TEXTURE_2D, src_tex);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   for (int i = 0; i < TW * TH; ++i) { pix[4*i] = 200; pix[4*i+1] = 100; pix[4*i+2] = 50; pix[4*i+3] = 255; }
   glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, TW, TH, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pix);

   const unsigned char bg[4] = {0, 0, 0, 255};
   const unsigned char texel[4] = {50, 100, 200, 255}; /* BGRA 200,100,50 -> RGBA 50,100,200 */
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   int fails = 0;
   for (int fr = 0; fr < frames && fails < 5; ++fr) {
      if (upload) {
         int rows = 1 + fr % 40, y = (fr * 37) % (TH - rows);
         glBindTexture(GL_TEXTURE_2D, src_tex);
         glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, TW, rows, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pix);
      }
      if (outside) {
         glBindFramebuffer(GL_FRAMEBUFFER, small);
         glViewport(0, 0, 64, 64);
         glUseProgram(solid);
         glUniform4f(glGetUniformLocation(solid, "col"), 1, 0, 0, 1);
         glEnable(GL_SCISSOR_TEST);
         glScissor(200 + fr % 50, 300, 40, 40);   /* fully outside 64x64 */
         quad(-1, -1, 1, 1);
         glDisable(GL_SCISSOR_TEST);
         unsigned char px[4];
         glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); /* forces the batch */
      }
      glBindFramebuffer(GL_FRAMEBUFFER, work);
      glViewport(0, 0, W, H);
      glDisable(GL_SCISSOR_TEST);
      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT | (ds ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : 0));
      /* "background" */
      glUseProgram(solid);
      glUniform4f(glGetUniformLocation(solid, "col"), 0.1f, 0.2f, 0.3f, 1);
      quad(-1, -1, 1, -0.5f);
      /* "window": textured quad, optionally blended, optionally scissored */
      if (blend) {
         glEnable(GL_BLEND);
         glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      }
      glUseProgram(texp);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, src_tex);
      glUniform1i(glGetUniformLocation(texp, "tex"), 0);
      glUniform1f(glGetUniformLocation(texp, "alpha"), 1.0f);
      if (scis) {
         glEnable(GL_SCISSOR_TEST);
         for (int r = 0; r < 4; ++r) {
            int sx = (fr * 13 + r * 97) % (W - 64), sy = (fr * 29 + r * 211) % (H - 64);
            glScissor(sx, sy, 17 + r * 23, 9 + r * 31);
            quad(-0.5f, -0.25f, 0.5f, 0.75f);
         }
         glScissor(W / 4 + 1, H * 3 / 8 + 1, W / 2 - 2, H / 2 - 2);
      }
      quad(-0.5f, -0.25f, 0.5f, 0.75f);
      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_BLEND);
      fails += check(W / 2, H / 2, texel, fr, "work-window");
      fails += check(2, 2, (const unsigned char[4]){26, 51, 77, 255}, fr, "work-bg");
      fails += check(W / 2, H - 3, bg, fr, "work-clear");
      if (copy) {
         glBindFramebuffer(GL_FRAMEBUFFER, out);
         glViewport(0, 0, W, H);
         glUseProgram(texp);
         glBindTexture(GL_TEXTURE_2D, work_tex);
         if (scis) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, W, H / 2 + 1 + (fr % 7));
         }
         quad(-1, -1, 1, 1);
         glDisable(GL_SCISSOR_TEST);
         fails += check(W / 2, H / 2, texel, fr, "out-window");
      }
   }
   clock_gettime(CLOCK_MONOTONIC, &t1);
   double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
   printf("%s: %d frames %.1f ms/frame\n", fails ? "FAIL" : "PASS", frames, s * 1000 / frames);
   return fails ? 1 : 0;
}
