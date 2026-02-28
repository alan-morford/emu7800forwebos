/*
 * OpenGL ES stub functions for cross-compilation
 * Real implementations provided by device at runtime
 */

void glEnable(unsigned int cap) { (void)cap; }
void glDisable(unsigned int cap) { (void)cap; }
void glClearColor(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; }
void glClear(unsigned int mask) { (void)mask; }
void glViewport(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void glMatrixMode(unsigned int mode) { (void)mode; }
void glLoadIdentity(void) { }
void glOrthof(float l, float r, float b, float t, float n, float f) { (void)l; (void)r; (void)b; (void)t; (void)n; (void)f; }
void glGenTextures(int n, unsigned int *textures) { (void)n; (void)textures; }
void glDeleteTextures(int n, const unsigned int *textures) { (void)n; (void)textures; }
void glBindTexture(unsigned int target, unsigned int texture) { (void)target; (void)texture; }
void glTexParameteri(unsigned int target, unsigned int pname, int param) { (void)target; (void)pname; (void)param; }
void glTexImage2D(unsigned int target, int level, int internalformat, int width, int height, int border, unsigned int format, unsigned int type, const void *pixels) { (void)target; (void)level; (void)internalformat; (void)width; (void)height; (void)border; (void)format; (void)type; (void)pixels; }
void glTexSubImage2D(unsigned int target, int level, int xoffset, int yoffset, int width, int height, unsigned int format, unsigned int type, const void *pixels) { (void)target; (void)level; (void)xoffset; (void)yoffset; (void)width; (void)height; (void)format; (void)type; (void)pixels; }
void glEnableClientState(unsigned int array) { (void)array; }
void glDisableClientState(unsigned int array) { (void)array; }
void glVertexPointer(int size, unsigned int type, int stride, const void *pointer) { (void)size; (void)type; (void)stride; (void)pointer; }
void glTexCoordPointer(int size, unsigned int type, int stride, const void *pointer) { (void)size; (void)type; (void)stride; (void)pointer; }
void glColorPointer(int size, unsigned int type, int stride, const void *pointer) { (void)size; (void)type; (void)stride; (void)pointer; }
void glDrawArrays(unsigned int mode, int first, int count) { (void)mode; (void)first; (void)count; }
void glColor4f(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; }
void glBlendFunc(unsigned int sfactor, unsigned int dfactor) { (void)sfactor; (void)dfactor; }
void glFinish(void) { }
void glFlush(void) { }
