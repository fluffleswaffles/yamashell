// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "yamashell.h"
#include <EGL/eglext.h>

#ifndef YAMASHELL_HOST
extern EGLBoolean vcglPresent(EGLDisplay, EGLSurface, EGLSurface);
/* libvcgl's native window (vc-khrn gl.h) */
struct vc_window{int x, y, width, height;};
#endif

static GLuint shader(GLenum type, const char *source){
	GLuint id=glCreateShader(type);
	GLint ok;
	glShaderSource(id, 1, &source, NULL);
	glCompileShader(id);
	glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
	if(!ok){fprintf(stderr, "yamashell: shader compilation failed\n"); glDeleteShader(id); return 0;}
	return id;
}

bool renderer_init(struct server *s){
	const EGLint config[]={EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
	const EGLint context[]={EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	const EGLint size[]={EGL_WIDTH, screen_w, EGL_HEIGHT, screen_h, EGL_NONE};
	EGLConfig cfg;
	EGLint n, ok;
	GLuint vs=0, fs=0;
#ifdef YAMASHELL_HOST
	s->egl_display=eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
#else
	s->egl_display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
#endif
	if(!eglInitialize(s->egl_display, NULL, NULL)){return false;}
	eglBindAPI(EGL_OPENGL_ES_API);
	if(!eglChooseConfig(s->egl_display, s->headless ? config : NULL, &cfg, 1, &n) || n!=1){goto fail;}
	s->egl_context=eglCreateContext(s->egl_display, cfg, EGL_NO_CONTEXT, context);
	if(s->egl_context==EGL_NO_CONTEXT){goto fail;}
#ifdef YAMASHELL_HOST
	s->egl_surface=eglCreatePbufferSurface(s->egl_display, cfg, size);
#else
	(void)size;
	static struct vc_window win;
	win=(struct vc_window){0, 0, screen_w, screen_h};
	s->egl_surface=eglCreateWindowSurface(s->egl_display, cfg, (EGLNativeWindowType)&win, NULL);
	s->egl_front=eglCreateWindowSurface(s->egl_display, cfg, (EGLNativeWindowType)&win, NULL);
	if(s->egl_front==EGL_NO_SURFACE){goto fail;}
#endif
	if(s->egl_context==EGL_NO_CONTEXT || s->egl_surface==EGL_NO_SURFACE ||
		!eglMakeCurrent(s->egl_display, s->egl_surface, s->egl_surface, s->egl_context)){goto fail;}
	vs=shader(GL_VERTEX_SHADER, "attribute vec2 pos; attribute vec2 uv; uniform vec4 bounds; varying vec2 texcoord; void main(){gl_Position=vec4(bounds.xy+pos*bounds.zw,0.,1.); texcoord=uv;}");
	fs=shader(GL_FRAGMENT_SHADER, "precision mediump float; uniform sampler2D tex; uniform float opaque; uniform float native; varying vec2 texcoord; void main(){vec4 c=texture2D(tex,texcoord); c=mix(c.bgra,c,native); gl_FragColor=vec4(c.rgb,mix(c.a,1.,opaque));}");
	if(!vs || !fs){goto fail;}
	s->program=glCreateProgram();
	glAttachShader(s->program, vs); glAttachShader(s->program, fs);
	glBindAttribLocation(s->program, 0, "pos"); glBindAttribLocation(s->program, 1, "uv");
	glLinkProgram(s->program); glGetProgramiv(s->program, GL_LINK_STATUS, &ok);
	if(ok){
		GLuint opaque=shader(GL_FRAGMENT_SHADER,"precision mediump float; uniform sampler2D tex; varying vec2 texcoord; void main(){gl_FragColor=texture2D(tex,texcoord);}");
		if(opaque){
			s->opaque_program=glCreateProgram();
			glAttachShader(s->opaque_program,vs);glAttachShader(s->opaque_program,opaque);
			glBindAttribLocation(s->opaque_program,0,"pos");glBindAttribLocation(s->opaque_program,1,"uv");
			glLinkProgram(s->opaque_program);glGetProgramiv(s->opaque_program,GL_LINK_STATUS,&ok);
			glDeleteShader(opaque);
		}else{ok=0;}
	}
	glDeleteShader(vs); glDeleteShader(fs);vs=fs=0;
	if(!ok){goto fail;}
	glUseProgram(s->opaque_program);
	s->opaque_bounds=glGetUniformLocation(s->opaque_program,"bounds");
	glUniform1i(glGetUniformLocation(s->opaque_program,"tex"),0);
	glUseProgram(s->program);
	s->alpha=glGetUniformLocation(s->program, "opaque");
	s->native=glGetUniformLocation(s->program, "native");
	s->bounds=glGetUniformLocation(s->program, "bounds");
	const GLfloat quad[]={0,0,0,0, 1,0,1,0, 0,1,0,1, 1,1,1,1};
	glGenBuffers(1,&s->quad);
	glBindBuffer(GL_ARRAY_BUFFER,s->quad);
	glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER,0);
	glUniform1i(glGetUniformLocation(s->program, "tex"), 0);
	glActiveTexture(GL_TEXTURE0);
	glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
	glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glViewport(0, 0, screen_w, screen_h);
	fprintf(stderr, "yamashell: renderer %s\n", glGetString(GL_RENDERER));
	if(glGetError()!=GL_NO_ERROR){goto fail;}
	return true;
fail:
	if(vs){glDeleteShader(vs);}if(fs){glDeleteShader(fs);}
	renderer_finish(s);return false;
}

bool renderer_upload(struct surface *v, struct wl_shm_buffer *b, struct rect r, bool fresh, bool opaque){
	uint32_t started=milliseconds();
	int width=wl_shm_buffer_get_width(b), height=wl_shm_buffer_get_height(b), stride=wl_shm_buffer_get_stride(b);
	unsigned char *pixels, *packed=NULL;
	bool rgb565=opaque;
#ifdef YAMASHELL_HOST
	rgb565=rgb565 && getenv("YAMASHELL_RGB565")!=NULL;
#endif
	GLenum format=rgb565 ? GL_RGB : GL_RGBA,type=rgb565 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE;
	if(width<=0 || height<=0 || width>4096 || height>4096 || stride<width*4){return false;}
	if(fresh){r=(struct rect){0, 0, width, height};}
	if(r.x<0 || r.y<0 || r.w<0 || r.h<0 || r.x>width-r.w || r.y>height-r.h){return false;}
	if(!v->texture){glGenTextures(1, &v->texture);}
	glBindTexture(GL_TEXTURE_2D, v->texture);
	if(fresh){
		/* linear: same pixels at 1:1, smooth task switcher cards */
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, type, NULL);
	}
	if(!r.w || !r.h){return glGetError()==GL_NO_ERROR;}
	size_t pitch=rgb565 ? ((size_t)r.w*2+3)&~(size_t)3 : (size_t)r.w*4;
	if(rgb565 || stride!=r.w*4){packed=calloc((size_t)r.h,pitch); if(!packed){return false;}}
	wl_shm_buffer_begin_access(b);
	pixels=(unsigned char *)wl_shm_buffer_get_data(b)+(size_t)r.y*stride+r.x*4;
	if(rgb565){
		for(int y=0;y<r.h;y++){
			const unsigned char *src=pixels+(size_t)y*stride;
			uint16_t *dst=(uint16_t *)(packed+(size_t)y*pitch);
			for(int x=0;x<r.w;x++,src+=4){dst[x]=((src[2]&0xf8)<<8)|((src[1]&0xfc)<<3)|(src[0]>>3);}
		}
	}else if(packed){for(int y=0;y<r.h;y++){memcpy(packed+(size_t)y*pitch,pixels+(size_t)y*stride,(size_t)r.w*4);}}
	if(packed){pixels=packed;}
	glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h, format, type, pixels);
	wl_shm_buffer_end_access(b);
	free(packed);
	v->rgb565=rgb565;
	v->server->uploaded+=(uint64_t)pitch*r.h;
	v->server->uploads++;v->server->upload_ms+=(uint32_t)(milliseconds()-started);
	return glGetError()==GL_NO_ERROR;
}

void renderer_quad(struct server *s, float l, float t, float r, float b, bool opaque){
	if(opaque){glUseProgram(s->opaque_program);glDisable(GL_BLEND);}
	glUniform4f(opaque ? s->opaque_bounds : s->bounds,l,t,r-l,b-t);
	glBindBuffer(GL_ARRAY_BUFFER,s->quad);
	glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(const void *)0);
	glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(const void *)(2*sizeof(float)));
	glDrawArrays(GL_TRIANGLE_STRIP,0,4);
	glBindBuffer(GL_ARRAY_BUFFER,0);
	if(opaque){glEnable(GL_BLEND);glUseProgram(s->program);}
}

static void draw(struct surface *v){
	int64_t x, y;
	surface_position(v, &x, &y);
	x+=v->content_x; y+=v->content_y;
	struct server *s=v->server;
	float x0=x, y0=y, x1=x+v->width/v->scale, y1=y+v->height/v->scale;
	if(s->zoom!=1){
		x0=s->zoom_x+(x0-s->zoom_x)*s->zoom; x1=s->zoom_x+(x1-s->zoom_x)*s->zoom;
		y0=s->zoom_y+(y0-s->zoom_y)*s->zoom; y1=s->zoom_y+(y1-s->zoom_y)*s->zoom;
	}
	float l=2.f*x0/screen_w-1.f, r=2.f*x1/screen_w-1.f;
	float t=1.f-2.f*y0/screen_h, b=1.f-2.f*y1/screen_h;
	glBindTexture(GL_TEXTURE_2D, v->texture);
	glUniform1f(v->server->alpha, v->opaque ? 1.f : 0.f);
	glUniform1f(v->server->native, v->rgb565 ? 1.f : 0.f);
	renderer_quad(v->server,l,t,r,b,v->opaque && v->rgb565);
}

/* a window's texture anywhere on screen, in pixels: the task switcher's cards */
void renderer_thumb(struct surface *v, float x, float y, float w, float h){
	struct server *s=v->server;
	if(!v->texture){return;}
	glBindTexture(GL_TEXTURE_2D, v->texture);
	glUniform1f(s->alpha, v->opaque ? 1.f : 0.f);
	glUniform1f(s->native, v->rgb565 ? 1.f : 0.f);
	renderer_quad(s,2.f*x/screen_w-1.f,1.f-2.f*y/screen_h,2.f*(x+w)/screen_w-1.f,1.f-2.f*(y+h)/screen_h,v->opaque && v->rgb565);
}

bool renderer_frame(struct server *s){
	struct surface *v;
	uint32_t started=milliseconds();
#ifndef YAMASHELL_HOST
	if(!eglMakeCurrent(s->egl_display,s->egl_surface,s->egl_surface,s->egl_context)){return false;}
#endif
	glClearColor(.04f, .06f, .08f, 1.f); glClear(GL_COLOR_BUFFER_BIT);
	if(ui_draw_under(s)){
		wl_list_for_each(v, &s->surfaces, link){
			if(!v->cursor && surface_visible(v)){draw(v);}
		}
		if(s->cursor && s->cursor->mapped && s->pointer_focus && wl_resource_get_client(s->cursor->resource)==wl_resource_get_client(s->pointer_focus->resource)){draw(s->cursor);}
	}
	ui_draw_over(s);
#ifdef YAMASHELL_HOST
	if(s->capture){
		char path[PATH_MAX];
		if(snprintf(path,sizeof(path),"%s.tmp",s->capture)>=(int)sizeof(path)){return false;}
		unsigned char *pixels=malloc(screen_w*screen_h*4);
		FILE *f=fopen(path, "wb");
		if(!pixels || !f){free(pixels); if(f){fclose(f);} return false;}
		glReadPixels(0, 0, screen_w, screen_h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		fprintf(f, "P6\n%d %d\n255\n", screen_w, screen_h);
		for(int y=screen_h-1; y>=0; y--){for(int x=0; x<screen_w; x++){fwrite(pixels+(y*screen_w+x)*4, 1, 3, f);}}
		if(fclose(f) || rename(path,s->capture)){free(pixels);return false;} free(pixels);
	}
#endif
#ifdef YAMASHELL_HOST
	if(!eglSwapBuffers(s->egl_display,s->egl_surface)){return false;}
#else
	if(!vcglPresent(s->egl_display,s->egl_surface,s->egl_front)){return false;}
	EGLSurface back=s->egl_front;s->egl_front=s->egl_surface;s->egl_surface=back;
#endif
	if(glGetError()!=GL_NO_ERROR){return false;}
	s->present_ms+=(uint32_t)(milliseconds()-started);
	s->frames++;
	return true;
}

void renderer_drop(struct surface *v){
	if(v->texture){glDeleteTextures(1, &v->texture); v->texture=0;}
}
void renderer_finish(struct server *s){
	if(s->quad){glDeleteBuffers(1,&s->quad);}
	if(s->program){glDeleteProgram(s->program);}
	if(s->opaque_program){glDeleteProgram(s->opaque_program);}
	eglMakeCurrent(s->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if(s->egl_front!=EGL_NO_SURFACE){eglDestroySurface(s->egl_display,s->egl_front);}
	if(s->egl_surface!=EGL_NO_SURFACE){eglDestroySurface(s->egl_display, s->egl_surface);}
	if(s->egl_context!=EGL_NO_CONTEXT){eglDestroyContext(s->egl_display, s->egl_context);}
	eglTerminate(s->egl_display);
}