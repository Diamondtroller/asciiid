// nvbug.cpp : Defines the entry point for the console application.
//

#include <wchar.h>
#include <stdio.h>

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

#include <string.h>

#include "gl.h"

#include "rgba8.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h" // beta: ImGuiItemFlags_Disabled

#include "imgui_impl_opengl3.h"

#include "asciiid_platform.h"
#include "asciiid_term.h"

#include "texheap.h"
#include "terrain.h"
#include "mesh.h"

#include "asciiid_urdo.h"

#include "matrix.h"

#include "fast_rand.h"


// just for write(fd)
#include <unistd.h>


A3D_VT* term = 0;

#define MOUSE_QUEUE

#ifdef MOUSE_QUEUE
struct MouseQueue
{
	int x, y;
	MouseInfo mi;
};

int mouse_queue_len=0;
const int mouse_queue_size = 256; // enough to handle 15K samples / sec
MouseQueue mouse_queue[mouse_queue_size];
#endif

ImFont* pFont = 0;
char ini_path[4096];

TermScreen* screen = 0;
Terrain* terrain = 0;
World* world = 0;
Mesh* active_mesh;

struct MeshPrefs
{
	// float pre_trans[3];
	float scale_val[3];
	float scale_rnd[3];
	float rotate_locZ_val;
	float rotate_locZ_rnd;
	float rotate_XY_val[2];
	float rotate_XY_rnd[2];
	// float translate_val[3];
	// float translate_rnd[3];
	float rotate_align;
};

int fonts_loaded = 0;
int palettes_loaded = 0;
GLuint pal_tex = 0;
uint8_t* ipal = 0;

void* GetMaterialArr();
void* GetPaletteArr();
void* GetFontArr();

struct Cell
{
	uint8_t fg[3];	// foreground color
	uint8_t gl;		// glyph code
	uint8_t bg[3];	// background color

	uint8_t flags;  
	// transparency mask :
	// 0x1 - fg 
	// 0x2 - gl 
	// 0x4 - bg

	// blend modes 3x3 bits:
	// 0x03 2-bits fg blend mode (0:replace, 1:multiply, 2:screen, 3:transparent)
	// 0x04 glyph write mask (0:replace, 1:keep)
	// 0x18 2-bits bg blend mode (0:replace, 1:multiply, 2:screen, 3:transparent)
	// 3 bits left!
};

struct MyMaterial
{
	static void Free()
	{
		glDeleteTextures(1,&tex);
	}

	static void Init()
	{
		MyMaterial* m = (MyMaterial*)GetMaterialArr();

		uint8_t g[3] = {'`',' ',','};
		uint8_t f[3] = {0xFF,0,0};
		for (int s=0; s<16; s++)
		{
			for (int r=0; r<3; r++)
			{
				m[0].shade[r][s].fg[0]=f[r];
				m[0].shade[r][s].fg[1]=f[r];
				m[0].shade[r][s].fg[2]=f[r];

				m[0].shade[r][s].gl = g[r];

				m[0].shade[r][s].bg[0]=0xCF;
				m[0].shade[r][s].bg[1]=0xCF;
				m[0].shade[r][s].bg[2]=0xCF;

				m[0].shade[r][s].flags = 0;
			}
		}

		for (int i = 1; i < 256; i++)
		{
			for (int r = 0; r < 3; r++)
			{
				for (int s = 0; s < 16; s++)
				{
					m[i].shade[r][s].bg[0] = fast_rand() & 0xFF;
					m[i].shade[r][s].bg[1] = fast_rand() & 0xFF;
					m[i].shade[r][s].bg[2] = fast_rand() & 0xFF;
					m[i].shade[r][s].fg[0] = fast_rand() & 0xFF;
					m[i].shade[r][s].fg[1] = fast_rand() & 0xFF;
					m[i].shade[r][s].fg[2] = fast_rand() & 0xFF;
					m[i].shade[r][s].gl = fast_rand() & 0xFF;
					m[i].shade[r][s].flags = 0;
				}
			}
		}

		glCreateTextures(GL_TEXTURE_2D, 1, &tex);

		glTextureStorage2D(tex, 1, GL_RGBA8UI, 128, 256);

		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTextureSubImage2D(tex, 0, 0, 0, 128, 256, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, m->shade );
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

		glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

	void Update()
	{
		MyMaterial* m = (MyMaterial*)GetMaterialArr();
		int y = (int)(this-m);
		// update this single material texture slice !
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTextureSubImage2D(tex, 0, 0, y, 128, 1, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, shade);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	}

	static GLuint tex; // single texture for all materials 128x256

	Cell shade[4][16]; // each cell has 2 texels !!! shade[3] is currently spare space.

	// althought we have only 16 cells, shade map has 7bits!
	// that makes timed shading 8x more precise spatialy :)
	// (last bit is left for elevation/transparency and depends on material mode)

//	int time_scale; // -80..-1 , 0 , +1..+80

	// TIMED SHADE_MAP EVALUATION:
	/*	
		uint64_t time64_usec = a4dGetTime();
		int cell; // = ???
		if (time_scale == 0)
		{
			cell = (shade_map >> 3) &0xF;
		}
		else
		{
			int mul_arr[] = { 470, 431, 395, 462, 332, 304, 279, 256 };
			int abs_scale;

			int multiplier;
			if (time_scale>0)
			{
				abs_scale = time_scale;
				multiplier = mul_arr[(abs_scale+6)&7];
			}
			else
			{
				abs_scale = -time_scale;
				multiplier = -mul_arr[(abs_scale+6)&7];
			}

			int shift = 30 - ( ( abs_scale + 6 ) >> 3 );

			cell = (( time64_usec * multiplier + (shade_map << (shift-3)) ) >> shift ) & 0xF;

			// so at every frame every material should cache (time64_usec * multiplier) >> (shift-3)
			// then during shading cell is simply = ((mat_cache + shade_map) >> 3 ) & 0xF
		}
	*/
};

GLuint MyMaterial::tex = 0;
MyMaterial mat[256];

struct MyPalette
{
	static void Init()
	{
		MyPalette* p = (MyPalette*)GetPaletteArr();
		for (int j = 0; j < 256; j++)
			for (int i = 0; i < 768; i++)
				p[j].rgb[i] = fast_rand() & 0xFF;
	}

	static bool Scan(A3D_DirItem item, const char* name, void* cookie)
	{
		if (!(item&A3D_FILE))
			return true;

		char buf[4096];
		snprintf(buf, 4095, "%s/%s", (char*)cookie, name);
		buf[4095] = 0;

		a3dLoadImage(buf, 0, MyPalette::Load);
		return true;
	}

	static void Load(void* cookie, A3D_ImageFormat f, int w, int h, const void* data, int palsize, const void* palbuf)
	{
		if (palettes_loaded == 256)
			return;

		MyPalette* p = (MyPalette*)GetPaletteArr() + palettes_loaded;

		uint32_t* buf = (uint32_t*)malloc(w*h * sizeof(uint32_t));
		Convert_UI32_AABBGGRR(buf, f, w, h, data, palsize, palbuf);

		// extract palette by sampling at centers of w/16 x h/16 patches
		int hx = (w + 16) / 32;
		int hy = (h + 16) / 32;

		for (int y = 0; y < 16; y++)
		{
			int row = w * (y * h / 16 + hy) + hx;
			for (int x = 0; x < 16; x++)
			{
				uint32_t rgb = buf[x * w / 16 + row];

				p->rgb[3 * (x + y * 16) + 0] = rgb & 0xFF;
				p->rgb[3 * (x + y * 16) + 1] = (rgb>>8) & 0xFF;
				p->rgb[3 * (x + y * 16) + 2] = (rgb>>16) & 0xFF;
			}
		}

		free(buf);
		palettes_loaded++;
	}

	uint8_t rgb[3 * 256];
} pal[256];

struct MyFont
{
	static bool Scan(A3D_DirItem item, const char* name, void* cookie)
	{
		if (!(item&A3D_FILE))
			return true;

		char buf[4096];
		snprintf(buf,4095,"%s/%s",(char*)cookie,name);
		buf[4095]=0;

		a3dLoadImage(buf, 0, MyFont::Load);
		return true;
	}

	static int Sort(const void* a, const void* b)
	{
		MyFont* fa = (MyFont*)a;
		MyFont* fb = (MyFont*)b;

		int qa = fa->width*fa->height;
		int qb = fb->width*fb->height;

		return qa - qb;
	}

	static void Free()
	{
		MyFont* fnt = (MyFont*)GetFontArr();
		for (int i=0; i<fonts_loaded; i++)
		{
			glDeleteTextures(1,&fnt[i].tex);
		}
	}

	static void Load(void* cookie, A3D_ImageFormat f, int w, int h, const void* data, int palsize, const void* palbuf)
	{
		if (fonts_loaded==256)
			return;
			
		MyFont* fnt = (MyFont*)GetFontArr() + fonts_loaded;

		fnt->width = w;
		fnt->height = h;

		int ifmt = GL_RGBA8;
		int fmt = GL_RGBA;
		int type = GL_UNSIGNED_BYTE;

		uint32_t* buf = (uint32_t*)malloc(w * h * sizeof(uint32_t));

		uint8_t rgb[3] = { 0xff,0xff,0xff };
		ConvertLuminance_UI32_LLZZYYXX(buf, rgb, f, w, h, data, palsize, palbuf);

		glCreateTextures(GL_TEXTURE_2D, 1, &fnt->tex);
		glTextureStorage2D(fnt->tex, 1, ifmt, w, h);

		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTextureSubImage2D(fnt->tex, 0, 0, 0, w, h, fmt, type, buf ? buf : data);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

		float white_transp[4] = { 1,1,1,0 };

		glTextureParameteri(fnt->tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTextureParameteri(fnt->tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTextureParameteri(fnt->tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTextureParameteri(fnt->tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);

		glTextureParameterfv(fnt->tex, GL_TEXTURE_BORDER_COLOR, white_transp);


		/*
		// if we want to filter font we'd have first to
		// modify 3 things in font sampling by shader:
		// - clamp uv to glyph boundary during sampling
		// - fade result by distance normalized to 0.5 of texel 
		//   between unclamped uv to clamping glyph boundary
		// - use manual lod as log2(font_zoom)

		int max_lod = 0;
		while (!((w & 1) | (h & 1)))
		{
			max_lod++;
			w >>= 1;
			h >>= 1;
		}
		glGenerateTextureMipmap(fnt->tex);
		glTextureParameteri(fnt->tex, GL_TEXTURE_MAX_LOD, max_lod);
		*/

		if (buf)
			free(buf);

		fonts_loaded++;

		qsort(GetFontArr(), fonts_loaded, sizeof(MyFont), MyFont::Sort);
	}

	void SetTexel(int x, int y, uint8_t val)
	{
		uint8_t texel[4] = { 0xFF,0xFF,0xFF,val };
		glTextureSubImage2D(tex, 0, x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, texel);
	}

	uint8_t GetTexel(int x, int y)
	{
		uint8_t texel[4];
		glGetTextureSubImage(tex, 0, x, y, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, 4, texel);
		return texel[3];
	}

	int width;
	int height;

	GLuint tex;
} font[256];

void* GetMaterialArr()
{
	return mat;
}

void* GetPaletteArr()
{
	return pal;
}

void* GetFontArr()
{
	return font;
}


int active_font = 0;
int active_glyph = 0x40; //@
int active_palette = 0;
int active_material = 0;

/*
float dawn_color[3] = { 1,.8f,0 };
float noon_color[3] = { 1,1,1 };
float dusk_color[3] = { 1,.2f,0 };
float midnight_color[3] = { .1f,.1f,.5f };
*/

float font_size = 10;// 0.125;// 16; // so every visual cell appears as 16px
float rot_yaw = 45;
float rot_pitch = 30;//90;

float inst_yaw = 0.0;
bool  inst_yaw_rnd = false;
float inst_pitch_avr = 0.0;
float inst_pitch_var = 0.0;
float inst_roll = 0.0;
bool  inst_added = false;

float lit_yaw = 45;
float lit_pitch = 30;//90;
float lit_time = 12.0f;
float ambience = 0.5;

bool spin_anim = false;
float pos_x = 0, pos_y = 0, pos_z = 0;
int mouse_in = 0;

int panning = 0;
int panning_x = 0;
int panning_y = 0;
double panning_dx = 0;
double panning_dy = 0;

float zoom_wheel = 0;

int spinning = 0;
int spinning_x = 0;
int spinning_y = 0;

int edit_mode = 0;
int creating = 0; // +1 = add, -1 = del
int painting = 0; 
const float STAMP_R = 0.50;
const float STAMP_A = 1.00;
float br_radius = 10;
float br_alpha = 0.5f;
int painting_x = 0;
int painting_y = 0;
double painting_dx;
double painting_dy;
double paint_dist;

bool diag_flipped = false;
bool br_limit = false;
int probe_z = 0;

uint64_t g_Time; // in microsecs

#define QUOT(a) #a
#define DEFN(a) "#define " #a " " QUOT(a) "\n"
#define CODE(...) #__VA_ARGS__

struct RenderContext
{
	void Create()
	{
		// meshes & bsp
		glCreateBuffers(1, &mesh_vbo);
		int mesh_face_size = 3*sizeof(float[3]) + sizeof(uint32_t); // 3*pos_xyz, visual
		glNamedBufferStorage(mesh_vbo, 1024 * mesh_face_size, 0, GL_DYNAMIC_STORAGE_BIT);

		glCreateVertexArrays(1, &mesh_vao);
		glBindVertexArray(mesh_vao);
		glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, mesh_face_size, (void*)0);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, mesh_face_size, (void*)((char*)0 + sizeof(float[3])));
		glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, mesh_face_size, (void*)((char*)0 + 2*sizeof(float[3])));
		glVertexAttribIPointer(3, 1, GL_UNSIGNED_INT,   mesh_face_size, (void*)((char*)0 + 3*sizeof(float[3])));

		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glEnableVertexAttribArray(2);
		glEnableVertexAttribArray(3);
		glBindVertexArray(0);

		const char* mesh_vs_src =
			CODE(#version 450\n)
			CODE(
				layout(location = 0) in vec3 a;
				layout(location = 1) in vec3 b;
				layout(location = 2) in vec3 c;
				layout(location = 3) in uint visual;

				uniform mat4 inst_tm;

				out vec3 va,vb,vc;
				out uint vis;
				void main()
				{
					va = (inst_tm * vec4(a, 1.0)).xyz;
					vb = (inst_tm * vec4(b, 1.0)).xyz;
					vc = (inst_tm * vec4(c, 1.0)).xyz;
					vis = visual;
				}
			);

		const char* mesh_gs_src =
			CODE(#version 450\n)
			CODE(
				layout(points) in;
				layout(triangle_strip, max_vertices = 3) out;				
				uniform mat4 tm;
				in vec3 va[];
				in vec3 vb[];
				in vec3 vc[];
				in uint vis[];

				flat out vec3 nrm;
				flat out uint matid;

				out float shade;
				out float elev;

				void main()
				{
					vec3 a = va[0];
					vec3 b = vb[0];
					vec3 c = vc[0];

					matid = vis[0] & 0xFF;
					nrm = normalize( cross( b-a, c-a ) );

					shade = float((vis[0] >> 8) & 0x7f) / 8.0;
					elev = float((vis[0] >> 15) & 0x1);
					gl_Position = tm * vec4(a, 1.0);
					EmitVertex();

					shade = float((vis[0] >> 16) & 0x7f) / 8.0;
					elev = float((vis[0] >> 23) & 0x1);
					gl_Position = tm * vec4(b, 1.0);
					EmitVertex();

					shade = float((vis[0] >> 24) & 0x7f) / 8.0;
					elev = float((vis[0] >> 31) & 0x1);
					gl_Position = tm * vec4(c, 1.0);
					EmitVertex();

					EndPrimitive();
				}
			);


		const char* mesh_fs_src =
			CODE(#version 450\n)
			CODE(
				uniform usampler2D m_tex;
				uniform sampler2D f_tex;
				uniform sampler3D p_tex;
				uniform vec4 lt;

				layout(location = 0) out vec4 color;

				flat in vec3 nrm;
				flat in uint matid;
				in float shade;
				in float elev;
				
				void main()
				{
					uint mat_x = 2 * (uint(round(shade)) & 0xF) + 32 * (uint(round(elev)) & 0x1);
					uvec4 fill_rgbc = texelFetch(m_tex, ivec2(0+mat_x, matid), 0);
					uvec4 fill_rgbp = texelFetch(m_tex, ivec2(1+mat_x, matid), 0);

					float glyph = 0.0; // at the moment pure bkgnd					
					color = vec4(mix(vec3(fill_rgbp.rgb), vec3(fill_rgbc.rgb), glyph) / 255.0, 1.0);

					vec3 light_pos = normalize(lt.xyz);
					float light = max(0.0, 0.5*lt.w + (1.0 - 0.5*lt.w)*dot(light_pos, normalize(nrm)));

					color.rgb *= light;

					// palettize
					color.rgb = texture(p_tex, color.xyz).rgb;
				}
			);

		const char* bsp_vs_src =
			CODE(#version 450\n)
			CODE(
				layout(location = 0) in vec3 a;
				layout(location = 1) in vec3 b;
				layout(location = 2) in vec3 c;

				out vec2 va,vb,vc;
				void main()
				{
					va = a.xy;
					vb = b.xy;
					vc = c.xy;
				}
			);

		const char* bsp_gs_src =
			CODE(#version 450\n)
			CODE(
				layout(points) in;
				layout(line_strip, max_vertices = 18) out;				
				uniform mat4 tm;
				in vec2 va[];
				in vec2 vb[];
				in vec2 vc[];

				void main()
				{
					vec2 x = va[0];
					vec2 y = vb[0];
					vec2 z = vc[0];

					vec4 v[8];
					for (int i=0; i<8; i++)
					{
						int ix = i&1;
						int iy = (i>>1)&1;
						int iz = (i>>2)&1;
						v[i] = tm * vec4(x[ix],y[iy],z[iz],1.0);
					}

					int quad[5] = int[5](0,1,3,2,0);

					// 2 quads
					for (int j=0; j<2; j++)
					{
						for (int i=0; i<5; i++)
						{
							gl_Position = v[quad[i]+4*j]; 
							EmitVertex();
						}
						EndPrimitive();
					}

					// 4 joints
					for (int i=0; i<4; i++)
					{
						gl_Position = v[i]; 
						EmitVertex();
						gl_Position = v[i+4]; 
						EmitVertex();
						EndPrimitive();
					}
				}
			);


		const char* bsp_fs_src =
			CODE(#version 450\n)
			CODE(

				layout(location = 0) out vec4 color;

				void main()
				{
					color = vec4(0,0,0,0.33);
				}
			);



		// patches
		glCreateBuffers(1, &vbo);
		glNamedBufferStorage(vbo, TERRAIN_TEXHEAP_CAPACITY * sizeof(GLint[5]), 0, GL_DYNAMIC_STORAGE_BIT);

		glCreateVertexArrays(1, &vao);
		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glVertexAttribIPointer(0, 4, GL_INT, sizeof(GLint[5]), (void*)0);
		glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(GLint[5]), (void*)sizeof(GLint[4]));
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glBindVertexArray(0);

		// ghost
		glCreateBuffers(1, &ghost_vbo);
		glNamedBufferStorage(ghost_vbo, sizeof(GLint[3*4*HEIGHT_CELLS]), 0, GL_DYNAMIC_STORAGE_BIT);

		glCreateVertexArrays(1, &ghost_vao);
		glBindVertexArray(ghost_vao);
		glBindBuffer(GL_ARRAY_BUFFER, ghost_vbo);
		glVertexAttribIPointer(0, 3, GL_INT, sizeof(GLint[3]), (void*)0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glEnableVertexAttribArray(0);
		glBindVertexArray(0);

		const char* ghost_vs_src =
			CODE(#version 450\n)
			DEFN(HEIGHT_SCALE)
			DEFN(HEIGHT_CELLS)
			DEFN(VISUAL_CELLS)
			CODE(
				layout(location = 0) in ivec3 xyz;
				uniform mat4 tm;
				void main()
				{
					float scale = float(VISUAL_CELLS) / float(HEIGHT_CELLS);
					vec4 pos = vec4(xyz, 1.0);
					pos.xy *= scale;
					gl_Position = tm * pos;
				}
			);

		const char* ghost_fs_src =
			CODE(#version 450\n)
			DEFN(HEIGHT_SCALE)
			DEFN(HEIGHT_CELLS)
			DEFN(VISUAL_CELLS)
			CODE(
				layout(location = 0) out vec4 color;
				uniform vec4 cl;
				void main()
				{
					color = cl;
				}
			);

		const char* vs_src = 
		CODE(#version 450\n)
		DEFN(HEIGHT_SCALE)
		DEFN(HEIGHT_CELLS)
		DEFN(VISUAL_CELLS)
		CODE(
			layout(location = 0) in ivec4 in_xyuv;
			layout(location = 1) in uint in_diag;
			out ivec4 xyuv;
			out uint diag;

			void main()
			{
				xyuv = in_xyuv;
				diag = in_diag;
			}
		);

		const char* gs_src = 
		CODE(#version 450\n)
		DEFN(HEIGHT_SCALE)
		DEFN(HEIGHT_CELLS)
		DEFN(VISUAL_CELLS)
		CODE(
			layout(points) in;
			layout(triangle_strip, max_vertices = 4*HEIGHT_CELLS*HEIGHT_CELLS ) out;

			uniform vec4 br;
			uniform usampler2D z_tex;
			uniform mat4 tm;

			uniform vec3 pr; // .x=height , .y=alpha (alpha=0.5 when probing, otherwise 1.0), .z is br_limit direction (+1/-1 or 0 if disabled)


			in ivec4 xyuv[];
			in uint diag[];

			out vec4 world_xyuv;
			out vec3 uvh;
			flat out vec3 normal;
			
			void main()
			{
				uint z;
				vec4 v;
				ivec2 xy;

				vec3 xyz[4];
				vec2 uv[4];

				float rvh = float(VISUAL_CELLS) / float(HEIGHT_CELLS);
				float dxy = 1.0 / float(HEIGHT_CELLS);
				ivec2 bxy = xyuv[0].xy*HEIGHT_CELLS;

				// todo: emit optimized strips
				// should allow having upto 6x6 patches -> 12 scalars * 6 strips * (6+1) cols * 2 verts = 1008 components (out of 1024)
				// currently max is 4x4 -> 12 scalars * 4*4 quads * 4 verts -> 768 components

				uint rot = diag[0];
				ivec4 order[2] = ivec4[2](ivec4(0, 1, 2, 3), ivec4(1, 3, 0, 2));

				for (int y = 0; y < HEIGHT_CELLS; y++)
				{
					for (int x = 0; x < HEIGHT_CELLS; x++)
					{
						xy = ivec2(x, y + 1);
						uv[0] = (xyuv[0].zw + vec2(xy) / HEIGHT_CELLS) * VISUAL_CELLS;
						z = texelFetch(z_tex, xyuv[0].zw*(HEIGHT_CELLS+1) + xy, 0).r;
						xy = bxy + xy*VISUAL_CELLS;
						xyz[0] = vec3(xy*dxy, z);

						xy = ivec2(x, y);
						uv[1] = (xyuv[0].zw + vec2(xy) / HEIGHT_CELLS) * VISUAL_CELLS;
						z = texelFetch(z_tex, xyuv[0].zw*(HEIGHT_CELLS + 1) + xy, 0).r;
						xy = bxy + xy*VISUAL_CELLS;
						xyz[1] = vec3(xy*dxy, z);

						xy = ivec2(x + 1, y + 1);
						uv[2] = (xyuv[0].zw + vec2(xy) / HEIGHT_CELLS) * VISUAL_CELLS;
						z = texelFetch(z_tex, xyuv[0].zw*(HEIGHT_CELLS + 1) + xy, 0).r;
						xy = bxy + xy * VISUAL_CELLS;
						xyz[2] = vec3(xy*dxy, z);

						xy = ivec2(x + 1, y);
						uv[3] = (xyuv[0].zw + vec2(xy) / HEIGHT_CELLS) * VISUAL_CELLS;
						z = texelFetch(z_tex, xyuv[0].zw*(HEIGHT_CELLS + 1) + xy, 0).r;
						xy = bxy + xy * VISUAL_CELLS;
						xyz[3] = vec3(xy*dxy, z);

						if (br.w != 0.0 && br.z>0 && br.w<=1.0 && br.w>=-1.0)
						{
							for (int i = 0; i < 4; i++)
							{
								vec2 d = xyz[i].xy - br.xy;
								float len = length(d);
								if (len < br.z)
								{
									float gauss = (0.5 + 0.5*cos(len/br.z*3.141592));

									int d = int(round(gauss*gauss * br.w * br.z * HEIGHT_SCALE));

									float z = xyz[i].z + d;

									if (pr.z!=0) // limit enabled
									{
										if (d > 0)
										{
											if (xyz[i].z > pr.x)
												z = xyz[i].z;
											else
											if (z > pr.x)
												z = pr.x;
										}
										else
										if (d < 0)
										{
											if (xyz[i].z < pr.x)
												z = xyz[i].z;
											else
											if (z < pr.x)
												z = pr.x;
										}
									}
									else
									{
										if (z < 0)
											z = 0;
										if (z > 0xffff)
											z = 0xffff;
									}

									xyz[i].z = z;

									// xyz[i].z += int(round(gauss*gauss * br.w * br.z * HEIGHT_SCALE));
									// xyz[i].z = clamp(xyz[i].z, 0, 0xffff);
								}
							}
						}

						vec3 norm[4];
						norm[0] = cross(xyz[1] - xyz[0], xyz[2] - xyz[0]);
						norm[1] = cross(xyz[2] - xyz[3], xyz[1] - xyz[3]);
						norm[2] = cross(xyz[3] - xyz[1], xyz[0] - xyz[1]);
						norm[3] = cross(xyz[0] - xyz[2], xyz[3] - xyz[2]);

						uint r = rot & 1;

						normal = norm[2 * r];
						normal.xy *= 1.0 / HEIGHT_SCALE;

						{
							int i = order[r][0];

							world_xyuv = vec4(xyz[i].xy, uv[i]);
							uvh.xyz = xyz[i] - ivec3(xyuv[0].xy, 0);
							uvh.xyz /= vec3(rvh, rvh, HEIGHT_SCALE);

							gl_Position = tm * vec4(xyz[i], 1.0);
							EmitVertex();
						}
						{
							int i = order[r][1];

							world_xyuv = vec4(xyz[i].xy, uv[i]);
							uvh.xyz = xyz[i] - ivec3(xyuv[0].xy, 0);
							uvh.xyz /= vec3(rvh, rvh, HEIGHT_SCALE);

							gl_Position = tm * vec4(xyz[i], 1.0);
							EmitVertex();
						}
						{
							int i = order[r][2];

							world_xyuv = vec4(xyz[i].xy, uv[i]);
							uvh.xyz = xyz[i] - ivec3(xyuv[0].xy, 0);
							uvh.xyz /= vec3(rvh, rvh, HEIGHT_SCALE);

							gl_Position = tm * vec4(xyz[i], 1.0);
							EmitVertex();
						}

						normal = norm[2 * r + 1];
						normal.xy *= 1.0 / HEIGHT_SCALE;

						{
							int i = order[r][3];

							world_xyuv = vec4(xyz[i].xy, uv[i]);
							uvh.xyz = xyz[i] - ivec3(xyuv[0].xy, 0);
							uvh.xyz /= vec3(rvh, rvh, HEIGHT_SCALE);

							gl_Position = tm * vec4(xyz[i], 1.0);
							EmitVertex();
						}


						rot = rot >> 1;
						EndPrimitive();
					}
				}
			}
		);

		const char* fs_src = 
		CODE(#version 450\n)
		DEFN(HEIGHT_SCALE)
		DEFN(HEIGHT_CELLS)
		DEFN(VISUAL_CELLS)
		CODE(
			layout(location = 0) out vec4 color;

			uniform usampler2D v_tex;
			uniform usampler2D m_tex;
			uniform sampler2D f_tex;
			uniform sampler3D p_tex;

			uniform vec4 lt; // light pos
			// uniform vec3 lc; // light rgb
			uniform vec4 br; // brush
			uniform vec3 qd; // quad diag (.z==1 height quad, .z==2 visual map quad)
			uniform vec3 pr; // .x=height , .y=alpha (alpha=0.5 when probing, otherwise 1.0), .z is br_limit direction (+1/-1 or 0 if disabled)
			uniform float fz; // font zoom

			uniform uint br_matid;

			flat in vec3 normal;
			in vec3 uvh;
			in vec4 world_xyuv;

			float Grid(vec2 d, vec2 p, float s)
			{
				d *= s;
				p = fract(p*s + vec2(0.5));

				float r = 1.0;

				if (d.x < 0.25)
				{
					float a = clamp(-log2(d.x * 4), 0.0, 1.0);
					float m = smoothstep(0.5 - d.x, 0.5, p.x) * smoothstep(0.5 + d.x, 0.5, p.x);
					r *= mix(1.0, pow(1.0 - m, 0.5), a);
				}
				if (d.y < 0.25)
				{
					float a = clamp(-log2(d.y * 4), 0.0, 1.0);
					float m = smoothstep(0.5 - d.y, 0.5, p.y) * smoothstep(0.5 + d.y, 0.5, p.y);
					r *= mix(1.0, pow(1.0 - m, 0.5), a);
				}

				return r;
			}
			
			void main()
			{
				// sample terrain visual
				uint visual = texelFetch(v_tex, ivec2(floor(world_xyuv.zw)), 0).r;
				//visual = 12345;

				vec3 light_pos = normalize(lt.xyz);
				float light = max(0.0, 0.5*lt.w + (1.0-0.5*lt.w)*dot(light_pos, normalize(normal)));

				{
					uint matid = visual & 0xFF;
					uint shade = (visual >> 8) & 0x7F;
					uint elev  = (visual >> 15) & 0x1;

					/*
					if (mode == 1) // replace shade with lighting
						shade = uint(round(light * 15.0));
					else
					if (mode == 2)
						shade = uint(round(light * shade));
					else
					if (mode == 3)
						shade = uint(round(light * 15.0)*(1 - shade) + shade);
					*/

					// if we're painting matid
					// replace matid if we're inside the brush
					if (br.w > 1.0)
					{
						// flat (no-alpha) matid brush
						float abs_r = abs(br.z);
						float len = length(world_xyuv.xy - br.xy);

						if (len<abs_r)
						{
							if (pr.z>0) // limit to above
							{
								if (uvh.z * HEIGHT_SCALE >= pr.x)
									matid = br_matid;
							}
							else
							if (pr.z<0) // limit to below
							{
								if (uvh.z * HEIGHT_SCALE < pr.x)
									matid = br_matid;
							}
							else // no z-limit
								matid = br_matid;
						}
					}

					/*
						we could define mode on 2 bits:
						- 0: use shade map than apply lighting to rgb (useful for sculpting w/o defined materials in editor)
						- 1: overwrite shade with lighting   \
						- 2: multiply shade map by lighting   >-- for game
						- 3: screen shade map with lighting  /
					*/

					// convert elev to 0,1,2 material row of shades
					elev = uint(1);

					// sample material array
					// y=0,1 -> descent; y=2,3 -> fill; y=4,5 -> ascent
					uint mat_x = 2 * shade + 32 * elev;
					uvec4 fill_rgbc = texelFetch(m_tex, ivec2(0+mat_x, matid), 0);
					uvec4 fill_rgbp = texelFetch(m_tex, ivec2(1+mat_x, matid), 0);

					//fill_rgbc.w = 44;

					uvec2 font_size = textureSize(f_tex,0);
					uvec2 glyph_size = font_size / 16;

					vec2 glyph_fract = fract(gl_FragCoord.xy * fz / glyph_size);
					glyph_fract.y = 1.0 - glyph_fract.y;
					if (glyph_fract.x < 0)
						glyph_fract.x += 1;
					if (glyph_fract.y < 0)
						glyph_fract.y += 1;
					if (glyph_fract.x >= 1)
						glyph_fract.x -= 1;
					if (glyph_fract.y >= 1)
						glyph_fract.y -= 1;

					// sample font texture (pure alpha)
					vec2 glyph_coord = vec2(fill_rgbc.w & 0xF, fill_rgbc.w >> 4);
					float glyph = texture(f_tex, (glyph_coord + glyph_fract) / 16.0).a;

					// compose glyph
					color = vec4(mix(vec3(fill_rgbp.rgb), vec3(fill_rgbc.rgb), glyph) / 255.0, 1.0);
					//color = vec4(glyph_fract, 0.5, 1.0);

					// if (mode == 0) // editing
					color.rgb *= light;
				}

				// palettize
				color.rgb = texture(p_tex, color.xyz).rgb;

				if (qd.z>0)
				{
					if (qd.z>1.5)
					{
						// matid probe
						vec2 pos = floor(world_xyuv.xy);
						if (pos == qd.xy)
						{
							color.rgb = mix(color.rgb, vec3(0, 0, 1), 0.25);
						}
					}
					else
					{
						// diagonal flip preview
						float d = float(VISUAL_CELLS) / float(HEIGHT_CELLS);
						if (world_xyuv.x >= qd.x && world_xyuv.x < qd.x + d &&
							world_xyuv.y >= qd.y && world_xyuv.y < qd.y + d)
						{
							//color.rb = mix(color.rb, color.rb * 0.5, qd.z);
							color.rgb = mix(color.rgb, vec3(0, 1, 0), 0.25);
						}
					}
				}
				else
				if (qd.z < 0)
				{
					float d = float(VISUAL_CELLS);
					// patch delete preview
					if (world_xyuv.x >= qd.x && world_xyuv.x < qd.x + d &&
						world_xyuv.y >= qd.y && world_xyuv.y < qd.y + d)
					{
						//color.rb = mix(color.rb, color.rb * 0.5, qd.z);
						color.rgb = mix(color.rgb, vec3(1, .2, 0), -qd.z*0.25);
					}
				}

				{
					// height probe

					if (uvh.z * HEIGHT_SCALE < pr.x)
					{
						//color.g *= (1.0 - 0.25 * pr.y);
						color.rgb = mix(color.rgb, vec3(0.25, 0.5, 0.75), 0.1 + 0.1 * pr.y);
					}

					if (pr.x>0)
					{
						float dz = 2.0 * fwidth(uvh.z) * HEIGHT_SCALE;
						float lo = smoothstep(-dz, 0, uvh.z * HEIGHT_SCALE - pr.x);
						float hi = smoothstep(+dz, 0, uvh.z * HEIGHT_SCALE - pr.x);
						float silh = lo*hi;
						color.rgb *= 1.0 - 0.5*silh*pr.y;
					}
				}

				if (!gl_FrontFacing)
					color.rgb = 0.25 * (vec3(1.0) - color.rgb);

				float dx = 1.25*length(vec2(dFdxFine(uvh.x), dFdyFine(uvh.x)));
				float dy = 1.25*length(vec2(dFdxFine(uvh.y), dFdyFine(uvh.y)));

				vec2 d = vec2(dx, dy);

				float grid = 1.0;
				grid = min(grid, Grid(d*1.50, uvh.xy, 1.0 / float(HEIGHT_CELLS)));
				grid = min(grid, Grid(d*1.25, uvh.xy, 1.0));
				grid = min(grid, Grid(d*1.00, uvh.xy, float(VISUAL_CELLS) / float(HEIGHT_CELLS)));

				// color.rgb *= grid;
				color.rgb = mix(vec3(0, 0, 1), color.rgb, grid);

				// brush preview
				if (br.w > 1.0)
				{
					// flat (no-alpha) matid brush
					float abs_r = abs(br.z);
					float len = length(world_xyuv.xy - br.xy);
					float alf = (abs_r - len) / abs_r;

					float dalf = fwidth(alf) * 2.0; // 2x thicker

					float lo = smoothstep(-dalf, 0, alf);
					float hi = smoothstep(+dalf, 0, alf);
					float silh =  lo * hi;

					color.rgb *= 1.0 - 0.5*silh; // bit stronger (was .25)
				}
				else
				if (br.w != 0.0)
				{
					float abs_r = abs(br.z);
					float len = length(world_xyuv.xy - br.xy);
					float alf = (abs_r - len) / abs_r;

					float dalf = fwidth(alf);
					float silh = smoothstep(-dalf, 0, alf) * smoothstep(+dalf, 0, alf);

					alf = max(0.0, alf);

					if (br.z>0)
						color.gb *= 1.0 - alf;
					else
						color.rg *= 1.0 - alf;

					color.rgb *= 1.0 - silh*0.25;
				}
			}
		);

		GLsizei loglen = 999;
		char logstr[1000];
		GLuint shader[3];



		GLenum bsp_st[3] = { GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER };
		const char* bsp_src[3] = { bsp_vs_src, bsp_gs_src, bsp_fs_src };
		bsp_prg = glCreateProgram();

		for (int i = 0; i < 3; i++)
		{
			shader[i] = glCreateShader(bsp_st[i]);
			GLint len = (GLint)strlen(bsp_src[i]);
			glShaderSource(shader[i], 1, &(bsp_src[i]), &len);
			glCompileShader(shader[i]);

			loglen = 999;
			glGetShaderInfoLog(shader[i], loglen, &loglen, logstr);
			logstr[loglen] = 0;

			if (loglen)
				printf("%s", logstr);

			glAttachShader(bsp_prg, shader[i]);
		}

		glLinkProgram(bsp_prg);

		for (int i = 0; i < 2; i++)
			glDeleteShader(shader[i]);

		loglen = 999;
		glGetProgramInfoLog(bsp_prg, loglen, &loglen, logstr);
		logstr[loglen] = 0;

		if (loglen)
			printf("%s", logstr);

		bsp_tm_loc = glGetUniformLocation(bsp_prg, "tm");


		GLenum mesh_st[3] = { GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER };
		const char* mesh_src[3] = { mesh_vs_src, mesh_gs_src, mesh_fs_src };
		mesh_prg = glCreateProgram();

		for (int i = 0; i < 3; i++)
		{
			shader[i] = glCreateShader(mesh_st[i]);
			GLint len = (GLint)strlen(mesh_src[i]);
			glShaderSource(shader[i], 1, &(mesh_src[i]), &len);
			glCompileShader(shader[i]);

			loglen = 999;
			glGetShaderInfoLog(shader[i], loglen, &loglen, logstr);
			logstr[loglen] = 0;

			if (loglen)
				printf("%s", logstr);

			glAttachShader(mesh_prg, shader[i]);
		}

		glLinkProgram(mesh_prg);

		for (int i = 0; i < 2; i++)
			glDeleteShader(shader[i]);

		loglen = 999;
		glGetProgramInfoLog(mesh_prg, loglen, &loglen, logstr);
		logstr[loglen] = 0;

		if (loglen)
			printf("%s", logstr);

		mesh_inst_tm_loc = glGetUniformLocation(mesh_prg, "inst_tm");
		mesh_tm_loc = glGetUniformLocation(mesh_prg, "tm");
		mesh_lt_loc = glGetUniformLocation(mesh_prg, "lt");
		mesh_m_tex_loc = glGetUniformLocation(mesh_prg, "m_tex");
		mesh_f_tex_loc = glGetUniformLocation(mesh_prg, "f_tex");
		mesh_p_tex_loc = glGetUniformLocation(mesh_prg, "p_tex");

		GLenum ghost_st[3] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
		const char* ghost_src[3] = { ghost_vs_src, ghost_fs_src };
		ghost_prg = glCreateProgram();

		for (int i = 0; i < 2; i++)
		{
			shader[i] = glCreateShader(ghost_st[i]);
			GLint len = (GLint)strlen(ghost_src[i]);
			glShaderSource(shader[i], 1, &(ghost_src[i]), &len);
			glCompileShader(shader[i]);

			loglen = 999;
			glGetShaderInfoLog(shader[i], loglen, &loglen, logstr);
			logstr[loglen] = 0;

			if (loglen)
				printf("%s", logstr);

			glAttachShader(ghost_prg, shader[i]);
		}

		glLinkProgram(ghost_prg);

		for (int i = 0; i < 2; i++)
			glDeleteShader(shader[i]);

		loglen = 999;
		glGetProgramInfoLog(ghost_prg, loglen, &loglen, logstr);
		logstr[loglen] = 0;

		if (loglen)
			printf("%s", logstr);

		ghost_tm_loc = glGetUniformLocation(ghost_prg, "tm");
		ghost_cl_loc = glGetUniformLocation(ghost_prg, "cl");

		prg = glCreateProgram();

		GLenum st[3] = { GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER };
		const char* src[3] = { vs_src, gs_src, fs_src };

		for (int i = 0; i < 3; i++)
		{
			shader[i] = glCreateShader(st[i]);
			GLint len = (GLint)strlen(src[i]);
			glShaderSource(shader[i], 1, &(src[i]), &len);
			glCompileShader(shader[i]);

			loglen = 999;
			glGetShaderInfoLog(shader[i], loglen, &loglen, logstr);
			logstr[loglen] = 0;

			if (loglen)
				printf("%s", logstr);

			glAttachShader(prg, shader[i]);
		}

		glLinkProgram(prg);

		for (int i = 0; i < 3; i++)
			glDeleteShader(shader[i]);

		loglen = 999;
		glGetProgramInfoLog(prg, loglen, &loglen, logstr);
		logstr[loglen] = 0;

		if (loglen)
			printf("%s", logstr);

		tm_loc = glGetUniformLocation(prg, "tm");
		z_tex_loc = glGetUniformLocation(prg, "z_tex");
		v_tex_loc = glGetUniformLocation(prg, "v_tex");
		m_tex_loc = glGetUniformLocation(prg, "m_tex");
		f_tex_loc = glGetUniformLocation(prg, "f_tex");
		p_tex_loc = glGetUniformLocation(prg, "p_tex");
		br_loc = glGetUniformLocation(prg, "br");
		qd_loc = glGetUniformLocation(prg, "qd");
		pr_loc = glGetUniformLocation(prg, "pr");
		lt_loc = glGetUniformLocation(prg, "lt");
		//lc_loc = glGetUniformLocation(prg, "lc");
		fz_loc = glGetUniformLocation(prg, "fz");
		br_matid_loc = glGetUniformLocation(prg, "br_matid");
	}

	void Delete()
	{
		glDeleteVertexArrays(1, &vao);
		glDeleteBuffers(1, &vbo);
		glDeleteProgram(prg);

		glDeleteVertexArrays(1, &ghost_vao);
		glDeleteBuffers(1, &ghost_vbo);
		glDeleteProgram(ghost_prg);

	}

	void PaintGhost(const double* tm, int px, int py, int pz, uint16_t ghost[4 * HEIGHT_CELLS])
	{
		GLint buf[3 * 4 * HEIGHT_CELLS];
		int g = 0, b = 0;

		px *= HEIGHT_CELLS;
		py *= HEIGHT_CELLS;

		for (int x = 0; x < HEIGHT_CELLS; x++)
		{
			buf[b++] = px + x;
			buf[b++] = py;
			buf[b++] = ghost[g++];
		}

		for (int y = 0; y < HEIGHT_CELLS; y++)
		{
			buf[b++] = px + HEIGHT_CELLS;
			buf[b++] = py + y;
			buf[b++] = ghost[g++];
		}

		for (int x = HEIGHT_CELLS; x > 0; x--)
		{
			buf[b++] = px + x;
			buf[b++] = py + HEIGHT_CELLS;
			buf[b++] = ghost[g++];
		}

		for (int y = HEIGHT_CELLS; y > 0; y--)
		{
			buf[b++] = px;
			buf[b++] = py + y;
			buf[b++] = ghost[g++];
		}

		float ftm[16];// NV bug! workaround
		for (int i = 0; i < 16; i++)
			ftm[i] = (float)tm[i];

		glBindVertexArray(ghost_vao);
		glUseProgram(ghost_prg);

		glUniformMatrix4fv(ghost_tm_loc, 1, GL_FALSE, ftm);

		glNamedBufferSubData(ghost_vbo, 0, sizeof(GLint[3 * 4 * HEIGHT_CELLS]), buf);

		glUniform4f(ghost_cl_loc, 0, 0, 0, 1.0f);
		glLineWidth(2.0f);
		glDrawArrays(GL_LINE_LOOP, 0, 4 * HEIGHT_CELLS);
		glLineWidth(1.0f);

		// flatten
		for (b = 0; b < 4 * HEIGHT_CELLS; b++)
			buf[3 * b + 2] = pz;
		glNamedBufferSubData(ghost_vbo, 0, sizeof(GLint[3 * 4 * HEIGHT_CELLS]), buf);

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		glUniform4f(ghost_cl_loc, 0, 0, 0, 0.2f);
		glDrawArrays(GL_TRIANGLE_FAN, 0, 4 * HEIGHT_CELLS);

		glDisable(GL_BLEND);

		glUseProgram(0);
		glBindVertexArray(0);
	}


	void BeginBSP(const double* tm)
	{
		float ftm[16];
		for (int i=0; i<16; i++)
			ftm[i] = (float)tm[i];

		glUseProgram(bsp_prg);

		glUniformMatrix4fv(bsp_tm_loc, 1, GL_FALSE, ftm);

		glBindVertexArray(mesh_vao);

		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_GEQUAL);
		glCullFace(GL_BACK);
		glDepthMask(0);

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		//glLineWidth(4.0f);

		mesh_faces=0;

		glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
	}

	static void RenderBSP(int level, const float bbox[6], void* cookie)
	{
		RenderContext* rc = (RenderContext*)cookie;
		
		float* buf = rc->mesh_map[rc->mesh_faces].abc;
		buf[0] = bbox[0];
		buf[1] = bbox[1];
		buf[3] = bbox[2];
		buf[4] = bbox[3];
		buf[6] = bbox[4];
		buf[7] = bbox[5];
		rc->mesh_faces++;

		if (rc->mesh_faces/* == 1024*/)
		{
			// flush
			glBufferSubData(GL_ARRAY_BUFFER, 0, rc->mesh_faces * sizeof(Face), rc->mesh_map);
			glDrawArrays(GL_POINTS, 0, rc->mesh_faces);
			rc->mesh_faces=0;
		}
	}

	void EndBSP()
	{
		if (mesh_faces)
		{
			// flush
			glBufferSubData(GL_ARRAY_BUFFER, 0, mesh_faces * sizeof(Face), mesh_map);
			glDrawArrays(GL_POINTS, 0, mesh_faces);
			mesh_faces=0;
		}

		glBindBuffer(GL_ARRAY_BUFFER, 0);

		glBindVertexArray(0);
		glUseProgram(0);

		glDisable(GL_CULL_FACE);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_BLEND);
		glDepthMask(1);
		//glLineWidth(1.0f);
	}	

	void BeginMeshes(const double* tm, const float* lt)
	{
		float ftm[16];
		for (int i=0; i<16; i++)
			ftm[i] = (float)tm[i];

		glUseProgram(mesh_prg);

		glUniformMatrix4fv(mesh_tm_loc, 1, GL_FALSE, ftm);
		glUniform4fv(mesh_lt_loc, 1, lt);
		glUniform1i(mesh_m_tex_loc, 2);
		glUniform1i(mesh_f_tex_loc, 3);
		glUniform1i(mesh_p_tex_loc, 4);

		glBindVertexArray(mesh_vao);

		glBindTextureUnit(2, MyMaterial::tex);
		glBindTextureUnit(3, font[active_font].tex);
		glBindTextureUnit(4, pal_tex);

//		glEnable(GL_CULL_FACE);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_GEQUAL);
		glCullFace(GL_BACK);

		//mesh_map=0;
		mesh_faces=0;

		glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
	}

	static void RenderFace(float coords[9], uint32_t visual, void* cookie)
	{
		RenderContext* rc = (RenderContext*)cookie;
		
		memcpy(rc->mesh_map[rc->mesh_faces].abc, coords, sizeof(float[9]));
		rc->mesh_map[rc->mesh_faces].visual = visual;
		rc->mesh_faces++;

		if (rc->mesh_faces == 1024)
		{
			// flush
			glBufferSubData(GL_ARRAY_BUFFER, 0, rc->mesh_faces * sizeof(Face), rc->mesh_map);
			glDrawArrays(GL_POINTS, 0, rc->mesh_faces);
			rc->mesh_faces=0;
		}
	}

	static void RenderMesh(Mesh* m, const double tm[16], void* cookie)
	{
		RenderContext* rc = (RenderContext*)cookie;

		if (rc->mesh_faces)
		{
			// flush
			glBufferSubData(GL_ARRAY_BUFFER, 0, rc->mesh_faces * sizeof(Face), rc->mesh_map);
			glDrawArrays(GL_POINTS, 0, rc->mesh_faces);
			rc->mesh_faces=0;
		}

		float ftm[16];
		for (int i=0; i<16; i++)
			ftm[i] = (float)tm[i];
		glUniformMatrix4fv(rc->mesh_inst_tm_loc, 1, GL_FALSE, ftm);
		QueryMesh(m, RenderFace, rc);
	}

	void EndMeshes()
	{
		if (mesh_faces)
		{
			// flush
			glBufferSubData(GL_ARRAY_BUFFER, 0, mesh_faces * sizeof(Face), mesh_map);
			glDrawArrays(GL_POINTS, 0, mesh_faces);
			mesh_faces=0;
		}

		glBindBuffer(GL_ARRAY_BUFFER, 0);

		glBindTextureUnit(2, 0);
		glBindTextureUnit(3, 0);
		glBindTextureUnit(4, 0);

		glBindVertexArray(0);
		glUseProgram(0);

		glDisable(GL_CULL_FACE);
		glDisable(GL_DEPTH_TEST);
	}

	void BeginPatches(const double* tm, const float* lt, const float* br, const float* qd, const float* pr)
	{
		glUseProgram(prg);

		static const float br_off[] = { 0,0,1,0 };
		if (!br)
			br = br_off;

		/*
		float* c1;
		float* c2;
		float w;
		if (lit_time < 6)
		{
			w = lit_time / 6.0f;
			c1 = midnight_color;
			c2 = dawn_color;
		}
		else
		if (lit_time < 12)
		{
			w = powf((lit_time-6) / 6.0f, 0.3f);
			c1 = dawn_color;
			c2 = noon_color;
		}
		else
		if (lit_time < 18)
		{
			w = 1.0f - powf(1.0f - (lit_time - 12) / 6.0f, 0.3f);
			c1 = noon_color;
			c2 = dusk_color;
		}
		else
		{
			w = (lit_time - 18) / 6.0f;
			c1 = dusk_color;
			c2 = midnight_color;
		}

		float lit_color[3];
		for (int c=0; c<3; c++)
			lit_color[c] = c1[c]*(1-w) + c2[c]*w;
		*/

		//glUniformMatrix4dv(tm_loc, 1, GL_FALSE, tm);
		float ftm[16];// NV bug! workaround
		for (int i = 0; i < 16; i++)
			ftm[i] = (float)tm[i];

		double font_zoom; // calc using lengths of diagonals

		font_zoom = font[active_font].width * font[active_font].width + font[active_font].height * font[active_font].height;
		font_zoom /= 512.0 * font_size * font_size; 
		font_zoom = sqrt(font_zoom);

		glUniformMatrix4fv(tm_loc, 1, GL_FALSE, ftm);
		glUniform4fv(lt_loc, 1, lt);
		//glUniform3fv(lc_loc, 1, lit_color);
		glUniform1i(z_tex_loc, 0);
		glUniform1i(v_tex_loc, 1);
		glUniform1i(m_tex_loc, 2);
		glUniform1i(f_tex_loc, 3);
		glUniform1i(p_tex_loc, 4);

		glUniform4fv(br_loc, 1, br);
		glUniform3fv(qd_loc, 1, qd);
		glUniform3fv(pr_loc, 1, pr);
		glUniform1f(fz_loc, (float)font_zoom);
		glUniform1ui(br_matid_loc, (GLuint)active_material);
		glBindVertexArray(vao);

		glBindTextureUnit(2, MyMaterial::tex);
		glBindTextureUnit(3, font[active_font].tex);
		glBindTextureUnit(4, pal_tex);

		head = 0;
		patches = 0;
		draws = 0;
		changes = 0;
		page_tex = 0;

		render_time = a3dGetTime();
	}

	static void RenderPatch(Patch* p, int x, int y, int view_flags, void* cookie)
	{
		RenderContext* rc = (RenderContext*)cookie;
		TexAlloc* ta = GetTerrainTexAlloc(p);

		rc->patches++;

		TexPageBuffer* buf = (TexPageBuffer*)ta->page->user;

		if (buf->size == 0)
		{
			if (rc->head)
				((TexPageBuffer*)rc->head->user)->prev = ta->page;
			buf->prev = 0;
			buf->next = rc->head;
			rc->head = ta->page;
		}

		GLint* patch = buf->data + 5 * buf->size;

		patch[0] = x;
		patch[1] = y;
		patch[2] = ta->x;
		patch[3] = ta->y;
		patch[4] = GetTerrainDiag(p);

		buf->size++;

		if (buf->size == TERRAIN_TEXHEAP_CAPACITY)
		{
			rc->draws++;
			
			if (rc->page_tex != ta->page)
			{
				rc->changes++;
				rc->page_tex = ta->page;

				for (int u=0; u<2; u++)
					glBindTextureUnit(u, rc->page_tex->tex[u]);
			}

			glNamedBufferSubData(rc->vbo, 0, sizeof(GLint[5]) * buf->size, buf->data);
			glDrawArrays(GL_POINTS, 0, buf->size);

			if (buf->prev)
				((TexPageBuffer*)buf->prev->user)->next = buf->next;
			else
				rc->head = buf->next;

			if (buf->next)
				((TexPageBuffer*)buf->next->user)->prev = buf->prev;

			buf->size = 0;
			buf->next = 0;
			buf->prev = 0;
		}

	}

	void EndPatches()
	{
		TexPage* tp = head;
		while (tp)
		{
			TexPageBuffer* buf = (TexPageBuffer*)tp->user;

			if (page_tex != tp)
			{
				changes++;
				page_tex = tp;

				for (int u=0; u<2; u++)
					glBindTextureUnit(u, page_tex->tex[u]);
			}

			draws++;
			glNamedBufferSubData(vbo, 0, sizeof(GLint[5]) * buf->size, buf->data);
			glDrawArrays(GL_POINTS, 0, buf->size);

			tp = buf->next;
			buf->size = 0;
			buf->next = 0;
			buf->prev = 0;
		}

		page_tex = 0;
		head = 0;

		for (int u = 0; u < 5; u++)
			glBindTextureUnit(u,0);

		glBindVertexArray(0);
		glUseProgram(0);

		render_time = a3dGetTime() - render_time;
	}

	GLint tm_loc; // uniform
	GLint lt_loc;
	//GLint lc_loc;
	GLint z_tex_loc;
	GLint v_tex_loc;
	GLint m_tex_loc;
	GLint f_tex_loc;
	GLint p_tex_loc;

	GLint br_loc;
	GLint qd_loc;
	GLint pr_loc;

	GLint fz_loc;
	GLint br_matid_loc;

	GLuint prg;
	GLuint vao;
	GLuint vbo;

	GLuint ghost_prg;
	GLuint ghost_vbo;
	GLuint ghost_vao;
	GLint ghost_tm_loc;
	GLint ghost_cl_loc;

	GLuint mesh_prg;
	GLuint mesh_vbo;
	GLuint mesh_vao;
	GLint mesh_inst_tm_loc;
	GLint mesh_tm_loc;
	GLint mesh_lt_loc;
	GLint mesh_m_tex_loc;
	GLint mesh_f_tex_loc;
	GLint mesh_p_tex_loc;

	GLuint bsp_prg;
	GLint bsp_tm_loc;

	int mesh_faces;
	struct Face
	{
		float abc[9];
		uint32_t visual;
	}; // * mesh_map;
	
	Face mesh_map[1024];

	TexPage* page_tex;
	TexPage* head;

	int patches; // rendered stats
	int draws;
	int changes;
	uint64_t render_time;
};

RenderContext render_context;

void GL_APIENTRY glDebugCall(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *userParam)
{
	static const char* source_str[] = // 0x8246 - 0x824B
	{
		"API",
		"WINDOW_SYSTEM",
		"SHADER_COMPILER",
		"THIRD_PARTY",
		"APPLICATION",
		"OTHER"
	};

	const char* src = "?";
	if (source >= 0x8246 && source <= 0x824B)
		src = source_str[source - 0x8246];

	static const char* type_str[] = // 0x824C - 0x8251
	{
		"ERROR",
		"DEPRECATED_BEHAVIOR",
		"UNDEFINED_BEHAVIOR",
		"PORTABILITY",
		"PERFORMANCE",
		"OTHER"
	};

	const char* typ = "?";
	if (type >= 0x824C && type <= 0x8251)
		typ = type_str[type - 0x824C];

	static const char* severity_str[] = // 0x9146 - 0x9148 , 0x826B
	{
		"HIGH",
		"MEDIUM",
		"LOW",
		"NOTIFICATION",
	};

	const char* sev = "?";
	if (severity >= 0x9146 && severity <= 0x9148)
		sev = severity_str[severity - 0x9146];
	else
		if (severity == 0x826B)
		{
			return;
			sev = severity_str[3];
		}

	printf("src:%s type:%s id:%d severity:%s\n%s\n\n", src, typ, id, sev, (const char*)message);
}

struct MatIDStamp
{
	static void SetMatCB(Patch* p, int x, int y, int view_flags, void* cookie)
	{
		MatIDStamp* t = (MatIDStamp*)cookie;

		double r2 = t->r * t->r;
		double* hit = t->hit;

		uint16_t* visual = GetTerrainVisualMap(p);

		bool diff = false;
		diff = true;
		URDO_Patch(p, true);

		for (int v = 0, i = 0; v < VISUAL_CELLS; v++)
		{
			for (int u = 0; u < VISUAL_CELLS; u++, i++)
			{
				double dx = u + x - hit[0];
				double dy = v + y - hit[1];
				if (dx*dx + dy * dy < r2)
				{
					int old = visual[i] & 0xFF;
					if (old != active_material)
					{
						if (t->z_lim > 0)
						{
							if (HitTerrain(p, (u + 0.5) / VISUAL_CELLS, (v + 0.5) / VISUAL_CELLS) < t->z)
								continue;
						}
						else
							if (t->z_lim < 0)
							{
								if (HitTerrain(p, (u + 0.5) / VISUAL_CELLS, (v + 0.5) / VISUAL_CELLS) >= t->z)
									continue;
							}

						if (!diff)
						{
							URDO_Patch(p, true);
							diff = true;
						}

						visual[i] = (visual[i] & ~0xFF) | active_material;
					}
				}
			}
		}

		if (diff)
			UpdateTerrainVisualMap(p);
	}

	int z_lim;
	double z;
	double r;
	double* hit;
};


struct Gather
{
	int x, y; // patch aligned
	int count; // number of actually queried patches
	int size; // in patches
	int* tmp_x;
	int* tmp_y;
	Patch* patch[1];

	int GetPatchIdx(int px, int py)
	{
		int dx = px - x;
		int dy = py - y;

		int bx = dx / VISUAL_CELLS;
		int by = dy / VISUAL_CELLS;

		assert(bx >= 0 && bx < size && by >= 0 && by < size);
		return bx + by * size;
	}

	int Sample(int hx, int hy) // hx and hy are in height map samples relative to Gather::x,y
	{
		int px = hx / HEIGHT_CELLS;
		int py = hy / HEIGHT_CELLS;

		int sx = hx % HEIGHT_CELLS;
		int sy = hy % HEIGHT_CELLS;

		int idx = px + py * size;
		Patch* p = patch[idx];

		if (!p)
			return -1;

		uint16_t* map = GetTerrainHeightMap(p);

		return map[sx + sy * (HEIGHT_CELLS + 1)];
	}
};



Gather* gather = 0;

static void GatherCB(Patch* p, int x, int y, int view_flags, void* cookie)
{
	gather->count++;
	gather->patch[gather->GetPatchIdx(x, y)] = p;
}

static void StampCB(Patch* p, int x, int y, int view_flags, void* cookie)
{
	double mul = br_alpha * br_radius * HEIGHT_SCALE;
	if (fabs(mul) < 0.499)
		return;

	uint16_t lo, hi;
	GetTerrainLimits(p, &lo, &hi);
	if (hi == 0 && br_alpha < 0 || lo == 0xffff && br_alpha>0)
		return;

	URDO_Patch(p);

	double* xy = (double*)cookie;
	uint16_t* map = GetTerrainHeightMap(p);

	const static double sxy = (double)VISUAL_CELLS / (double)HEIGHT_CELLS;

	double max_r2 = 0;

	for (int i=0, hy = 0; hy <= HEIGHT_CELLS; hy++)
	{
		double dy = y + sxy * hy - xy[1];
		dy *= dy;
		for (int hx = 0; hx <= HEIGHT_CELLS; hx++, i++)
		{
			double dx = x + sxy * hx - xy[0];
			dx *= dx;

			double len = sqrt(dx + dy);
			if (len < br_radius)
			{
				double gauss = 0.5 + 0.5*cos(len / br_radius * M_PI);

				int d = (int)(round(gauss*gauss * mul));
				if (d)
					max_r2 = fmax(max_r2, dx + dy);

				int z = map[i] + d;

				if (br_limit)
				{
					if (d > 0)
					{
						if (map[i] > probe_z)
							z = map[i];
						else
						if (z > probe_z)
							z = probe_z;
					}
					else
					if (d < 0)
					{
						if (map[i] < probe_z)
							z = map[i];
						else
						if (z < probe_z)
							z = probe_z;
					}
				}
				else
				{
					if (z < 0)
						z = 0;
					if (z > 0xffff)
						z = 0xffff;
				}
				map[i] = z;
			}
		}
	}

	xy[2] = fmax(xy[2], max_r2);
	UpdateTerrainHeightMap(p);
}

void Stamp(double x, double y)
{
	// query all patches int radial range br_xyra[2] from x,y
	// get their heightmaps apply brush on height samples and update TexHeap pages 

	ImGuiIO& io = ImGui::GetIO();

	int stamp_mode = 1;
	if (io.KeysDown[A3D_LSHIFT])
		stamp_mode = 2;

	if (stamp_mode == 1)
	{
		URDO_Open();
		double xy[3] = { x,y,0 };
		QueryTerrain(terrain, x, y, br_radius * 1.001, 0x00, StampCB, xy);
		URDO_Close();
	}
	else
	{
		double mul = br_alpha * br_radius * HEIGHT_SCALE;
		if (fabs(mul) < 0.499)
			return;

		// gather
		int size = 4 * (int)ceil(br_radius / VISUAL_CELLS) + 2;
		int tmp_buf_size = sizeof(int)*(size*HEIGHT_CELLS)*(size*HEIGHT_CELLS);
		if (!gather || gather->size != size)
		{
			if (gather)
			{
				free(gather->tmp_x);
				free(gather->tmp_y);
				free(gather);
			}
			int bs = sizeof(Gather) + sizeof(Patch*)*(size*size - 1);
			gather = (Gather*)malloc(bs);
			gather->size = size;

			gather->tmp_x = (int*)malloc(tmp_buf_size);
			gather->tmp_y = (int*)malloc(tmp_buf_size);
		}

		memset(gather->patch, 0, sizeof(Patch*)*(size*size));

		gather->x = (int)floor(x / VISUAL_CELLS - 0.5 * size) * VISUAL_CELLS;
		gather->y = (int)floor(y / VISUAL_CELLS - 0.5 * size) * VISUAL_CELLS;

		gather->count=0;
		QueryTerrain(terrain, x, y, 2.0*br_radius, 0x00, GatherCB, 0);

		if (!gather->count)
			return;

		int* tmp_x = gather->tmp_x;
		memset(tmp_x, -1, tmp_buf_size);

		int r = (int)floor(br_radius * HEIGHT_CELLS / VISUAL_CELLS);
		for (int hy = 0; hy < size * HEIGHT_CELLS; hy++)
		{
			for (int hx = r; hx < size * HEIGHT_CELLS - r; hx++)
			{
				double acc = 0;
				double den = 0;

				for (int sx = hx-r; sx < hx+r; sx++)
				{
					int h = gather->Sample(sx, hy);
					if (h >= 0)
					{
						// HERE we use TRUE gaussian filter (must be separable)
						double len = (double)sx * VISUAL_CELLS / HEIGHT_CELLS + gather->x - x;
						len /= br_radius;
						double gauss = exp(-len * len * 3);

						acc += h * gauss;
						den += gauss;
					}
				}

				if (den > 0)
					tmp_x[hx + hy * size * HEIGHT_CELLS] = (uint16_t)round(acc / den);
				else
					tmp_x[hx + hy * size * HEIGHT_CELLS] = -1;
			}
		}

		int* tmp_y = gather->tmp_y;
		memset(tmp_y, -1, tmp_buf_size);

		for (int hy = r; hy < size * HEIGHT_CELLS - r; hy++)
		{
			for (int hx = r; hx < size * HEIGHT_CELLS - r; hx++)
			{
				double acc = 0;
				double den = 0;

				for (int sy = hy - r; sy < hy + r; sy++)
				{
					int h = tmp_x[hx + sy * size * HEIGHT_CELLS];
					if (h >= 0)
					{
						// HERE we use TRUE gaussian filter (must be separable)
						double len = (double)sy * VISUAL_CELLS / HEIGHT_CELLS + gather->y - y;
						len /= br_radius;
						double gauss = exp(-len*len*3);

						acc += h * gauss;
						den += gauss;
					}
				}

				if (den > 0)
					tmp_y[hx + hy * size * HEIGHT_CELLS] = (uint16_t)round(acc / den);
				else
					tmp_y[hx + hy * size * HEIGHT_CELLS] = -1;
			}
		}

		// run all patches
		URDO_Open();
		for (int py = gather->size/4; py < gather->size - gather->size / 4; py++)
		{
			for (int px = gather->size / 4; px < gather->size - gather->size / 4; px++)
			{
				Patch* p = gather->patch[px + size * py];
				if (p)
				{
					URDO_Patch(p);
					uint16_t* map = GetTerrainHeightMap(p);

					for (int sy = 0; sy <= HEIGHT_CELLS; sy++)
					{
						int hy = (HEIGHT_CELLS * py + sy);
						double dy = gather->y + hy * VISUAL_CELLS / (double)HEIGHT_CELLS - y;
						dy *= dy;
						for (int sx = 0; sx <= HEIGHT_CELLS; sx++)
						{
							int hx = (HEIGHT_CELLS * px + sx);
							double dx = gather->x + hx * VISUAL_CELLS / (double)HEIGHT_CELLS - x;
							dx *= dx;

							double len = sqrt(dx + dy);

							if (len < br_radius)
							{
								double gauss = 0.5 + 0.5*cos(len / br_radius * M_PI);
								gauss *= gauss * br_alpha;

								if (gauss < 0)
								{
									double diff = gauss * (tmp_y[hx + hy * size * HEIGHT_CELLS] - map[sx + sy * (HEIGHT_CELLS + 1)]);
									int z = (int)round(diff) + map[sx + sy * (HEIGHT_CELLS + 1)];
									if (z < 0)
										z = 0;
									if (z > 0xffff)
										z = 0xffff;

									map[sx + sy * (HEIGHT_CELLS + 1)] = z;
								}
								else
								{
									double blend = map[sx + sy * (HEIGHT_CELLS + 1)] * (1.0 - gauss);
									blend += tmp_y[hx + hy * size * HEIGHT_CELLS] * gauss;
									map[sx + sy * (HEIGHT_CELLS + 1)] = (uint16_t)round(blend);
								}
							}
						}
					}

					UpdateTerrainHeightMap(p);
				}
			}
		}
		URDO_Close();
	}
}

void Palettize(const uint8_t p[768])
{
	if (!p && ipal)
	{
		free(ipal);
		ipal = 0;
	}
	else
	if (p && !ipal)
	{
		ipal = (uint8_t*)malloc(1<<24);
	}

	//glFinish();
	uint64_t t0 = a3dGetTime();

	GLuint vbo;
	glCreateBuffers(1, &vbo);
	float quad[8] = { 0,0,1,0,1,1,0,1 };
	glNamedBufferStorage(vbo, sizeof(float[2])*4, quad, 0);

	GLuint vao;
	glCreateVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float[2]), (void*)0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glBindVertexArray(0);

	GLuint prg;

	GLsizei loglen = 999;
	char logstr[1000];

	const char* vs_src =
		CODE(#version 450\n)
		CODE(
			layout(location = 0) in vec2 pos; // 0.0 - 1.0
			uniform float slice; // 0.0 - 255.0
			out vec3 fpos;       // 0.0-0.5/255 - 1.0+0.5/255
			void main()
			{
				float d0 = 0.0 - 0.5;
				float d1 = 255.0 + 0.5;
				fpos = vec3( mix(vec2(d0, d0), vec2(d1, d1), pos), slice );
				gl_Position = vec4(2.0*pos-vec2(1.0),0.0,1.0);
			}
		);

	const char* fs_src =
		CODE(#version 450\n)
		CODE(
			uniform uvec3 pal[256]; // 0 - 255
			uniform bool unpal;
			layout(location = 0) out vec4 lut;
			in vec3 fpos;
			void main()
			{
				if (unpal)
					lut = vec4(fpos / 255.0, 1.0);
				else
				{
					float diff = 100000000; // greater than max possible diff
					int idx = -1;

					// find closest color in palette
					for (int j = 0; j < 256; j++)
					{
						vec3 dd = fpos - vec3(pal[j]);
						dd *= dd;

						float d = max(max(fpos.r, fpos.g), fpos.b) - float(max(max(pal[j].r, pal[j].g), pal[j].b));
						d *= 16 * d; // mostly luminance
						d += 2 * dd.r + 4 * dd.g + 3 * dd.b; // bit of chrominance

						if (d < diff)
						{
							idx = j;
							diff = d;
						}
					}

					lut = vec4(vec3(pal[idx]) / 255.0, float(idx) / 255.0);
				}
			}
		);

	GLenum st[3] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	const char* src[3] = { vs_src, fs_src };
	prg = glCreateProgram();
	GLuint shader[3];

	for (int i = 0; i < 2; i++)
	{
		shader[i] = glCreateShader(st[i]);
		GLint len = (GLint)strlen(src[i]);
		glShaderSource(shader[i], 1, &(src[i]), &len);
		glCompileShader(shader[i]);

		loglen = 999;
		glGetShaderInfoLog(shader[i], loglen, &loglen, logstr);
		logstr[loglen] = 0;

		if (loglen)
			printf("%s", logstr);

		glAttachShader(prg, shader[i]);
	}

	glLinkProgram(prg);

	for (int i = 0; i < 2; i++)
		glDeleteShader(shader[i]);

	GLint slice_loc = glGetUniformLocation(prg,"slice");
	GLint pal_loc = glGetUniformLocation(prg, "pal");
	GLint unpal_loc = glGetUniformLocation(prg, "unpal");
	glUseProgram(prg);

	if (p)
	{
		GLuint uipal[768];
		for (int i = 0; i < 768; i++)
			uipal[i] = (GLuint)p[i];
		glUniform3uiv(pal_loc, 256, uipal);
		glUniform1i(unpal_loc, false);
	}
	else
		glUniform1i(unpal_loc, true);

	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	glBindVertexArray(vao);

	glViewport(0, 0, 256, 256);
	for (int slice = 0; slice < 256; slice++)
	{
		glFramebufferTexture3D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_3D, pal_tex, 0, slice);
		glUniform1f(slice_loc, (float)slice);
		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	}

	glDeleteFramebuffers(1, &fbo);
	glDeleteVertexArrays(1, &vao);
	glDeleteBuffers(1, &vbo);
	glDeleteProgram(prg);


	//glFinish();
	uint64_t t1 = a3dGetTime();
	printf("palettized in %d us\n", (int)(t1 - t0));

	if (ipal)
	{
		glGetTextureImage(pal_tex, 0, GL_ALPHA, GL_UNSIGNED_BYTE, 1<<24, ipal);
		uint64_t t2 = a3dGetTime();
		printf("fetched ipal in %d us\n", (int)(t2 - t1));
	}
}


struct DirItem
{
	A3D_DirItem item;
	DirItem* next;
	char name[1];
};

void FreeDir(DirItem** dir)
{
	DirItem** i = dir;
	while (*i)
	{
		free(*i);
		i++;
	}
	free(dir);
}

int AllocDir(DirItem*** dir, DirItem** list = 0)
{
	if (!dir)
		return -1;

	struct X
	{
		struct Head
		{
			int num;
			DirItem* list;
		};

		static int cmp(const void* a, const void* b)
		{
			const DirItem* p = *(const DirItem**)a;
			const DirItem* q = *(const DirItem**)b;

			if (p->item == A3D_DIRECTORY && q->item == A3D_FILE)
				return -1;
			if (p->item == A3D_FILE && q->item == A3D_DIRECTORY)
				return 1;
			return strcmp(p->name, q->name);
		}


		static bool Scan(A3D_DirItem item, const char* name, void* cookie)
		{
			Head* h = (Head*)cookie;
			DirItem* i = (DirItem*)malloc(sizeof(DirItem) + strlen(name));

			i->item = item;
			i->next = h->list;
			strcpy(i->name, name);
			h->list = i;
			h->num++;

			return true;
		}
	};

	X::Head head = { 0,0 };
	a3dListDir(".", X::Scan, &head);

	if (list)
		*list = head.list;

	DirItem* itm = head.list;
	DirItem** arr = (DirItem**)malloc(sizeof(DirItem*)*(head.num+1));
	for (int i = 0; i < head.num; i++)
	{
		arr[i] = itm;
		itm = itm->next;
	}

	qsort(arr, head.num, sizeof(DirItem*), X::cmp);

	arr[head.num] = 0;
	*dir = arr;

	return head.num;
}

void my_render()
{
	ImGuiIO& io = ImGui::GetIO();

	#ifdef MOUSE_QUEUE
	while (mouse_queue_len) // accumulate wheel sequence only
	{
		mouse_queue_len--;

		bool sync = false;

		int x = mouse_queue[0].x;
		int y = mouse_queue[0].y;
		MouseInfo mi = mouse_queue[0].mi;

		if ((mi & 0xF) == MouseInfo::LEAVE)
		{
			sync = true;
			mouse_in = 0;
		}
		else
		{
			if ((mi & 0xF) == MouseInfo::ENTER)
			{
				sync = true;
				mouse_in = 1;
			}

			io.MousePos = ImVec2((float)x, (float)y);

			switch (mi & 0xF)
			{
				case MouseInfo::WHEEL_DN:
					zoom_wheel--;
					io.MouseWheel -= 1.0;
					break;
				case MouseInfo::WHEEL_UP:
					zoom_wheel++;
					io.MouseWheel += 1.0;
					break;

				case MouseInfo::LEFT_DN:
					sync=true;
					io.MouseDown[0] = true;
					break;
				case MouseInfo::LEFT_UP:
					sync=true;
					io.MouseDown[0] = false;
					break;
				case MouseInfo::RIGHT_DN:
					sync=true;
					io.MouseDown[1] = true;
					break;
				case MouseInfo::RIGHT_UP:
					sync=true;
					io.MouseDown[1] = false;
					break;
				case MouseInfo::MIDDLE_DN:
					sync=true;
					io.MouseDown[2] = true;
					break;
				case MouseInfo::MIDDLE_UP:
					sync=true;
					io.MouseDown[2] = false;
					break;
			}
		}

		for (int i=0; i<mouse_queue_len; i++)
			mouse_queue[i] = mouse_queue[i+1];

		if (sync)
			break;
	}
	#endif

	// THINGZ
	const float clear_in[4]={0.45f, 0.55f, 0.60f, 1.00f};
	const float clear_out[4]={0.40f, 0.50f, 0.55f, 0.95f};

	//const float* clear_color = mouse_in ? clear_in : clear_out;
	const float* clear_color = clear_in;

	{
		ImGui_ImplOpenGL3_NewFrame();
		{
			// Setup time step
			ImGuiIO& io = ImGui::GetIO();
			uint64_t current_time = a3dGetTime();
			uint64_t delta = current_time - g_Time;
			io.DeltaTime = delta>0 ? delta / 1000000.0f : FLT_MIN;
			g_Time = current_time;
			// Start the frame
			ImGui::NewFrame();
		}


//		if (pFont)
//			ImGui::PushFont(pFont);		

//		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
//		ImGui::SetNextWindowPos(ImVec2(0,0),ImGuiCond_Always);
		//ImGui::SetNextWindowSizeConstraints(ImVec2(0,0),ImVec2(0,0),Dock::Size,0);
//		ImGui::PopStyleVar();

		struct MeshWidget
		{
			static void draw_cb(const ImDrawList* parent_list, const ImDrawCmd* cmd)
			{
				MeshWidget* mw = (MeshWidget*)cmd->UserCallbackData;
				if (!mw)
					return;

				int vp[4];
				glGetIntegerv(GL_VIEWPORT,vp);

				int sc[4];
				glGetIntegerv(GL_SCISSOR_BOX,sc);				

				int vao;
				glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vao);

				int vbo;
				glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vbo);

				int prg;
				glGetIntegerv(GL_CURRENT_PROGRAM,&prg);

				bool cull_face;
				cull_face = glIsEnabled(GL_CULL_FACE);

				int cull_mode;
				glGetIntegerv(GL_CULL_FACE_MODE, &cull_mode);

				int depth_func;
				glGetIntegerv(GL_DEPTH_FUNC, &depth_func);

				bool depth_test;
				depth_test = glIsEnabled(GL_DEPTH_TEST);

				glViewport(
					(int)mw->rect.Min.x, 
					vp[3] - (int)mw->rect.Max.y, 
					(int)(mw->rect.Max.x - mw->rect.Min.x), 
					(int)(mw->rect.Max.y - mw->rect.Min.y));

				glScissor(
					(int)mw->rect.Min.x, 
					vp[3] - (int)mw->rect.Max.y, 
					(int)(mw->rect.Max.x - mw->rect.Min.x), 
					(int)(mw->rect.Max.y - mw->rect.Min.y));

				float bbox[6];
				GetMeshBBox(active_mesh, bbox);

				float radius = 0.5f * sqrtf( (bbox[1]-bbox[0])*(bbox[1]-bbox[0]) + (bbox[3]-bbox[2])*(bbox[3]-bbox[2]) );
				// todo radius could be calculated from bounding circle on XY

				// radius = 0.5 * fmaxf( (bbox[1]-bbox[0]), (bbox[3]-bbox[2]) );

				float height = bbox[5]-bbox[4];
				float alpha = atan2f(2*radius,height);
				if (alpha < (float)M_PI/6)
					alpha = (float)M_PI/6;

				float x_proj = 2*radius;
				float y_proj = fmaxf(2*radius, height * cosf(alpha) + 2*radius*sinf(alpha));

				float box_aspect = x_proj / y_proj;
				float vue_aspect = (mw->rect.Max.x - mw->rect.Min.x) / (mw->rect.Max.y - mw->rect.Min.y);

				float s[3];

				if (box_aspect > vue_aspect)
				{
					// mesh is wider than view
					s[0] = 2.0f / x_proj;
					s[1] = s[0] * vue_aspect;
				}
				else
				{
					// mesh is taller than view
					s[1] = 2.0f / y_proj;
					s[0] = s[1] / vue_aspect;
				}

				// depth scaling, bit over estimated.
				s[2] = -2.0f / (bbox[5]-bbox[4] + bbox[3]-bbox[2] + bbox[1]-bbox[0]); 

				float vtm[16] = 
				{
					s[0], 0.0,  0.0,  0.0,
					0.0,  s[1], 0.0,  0.0,
					0.0,  0.0,  s[2], 0.0,
					0.0,  0.0,  0.0,  1.0
				};

				float t[3] =
				{
					-0.5f*(bbox[0]+bbox[1]),
					-0.5f*(bbox[2]+bbox[3]),
					-0.5f*(bbox[4]+bbox[5])
				};

				float trn[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, t[0], t[1], t[2], 1 };

				float rot1[16];
				float rot2[16];
				float v1[3] = {1,0,0};
				float v2[3] = {0,0,1};
				Rotation(v1, M_PI/180 * (rot_pitch-90), rot1);
				Rotation(v2, M_PI/180 * (-rot_yaw), rot2);

				float rot[16];
				MatProduct(rot1, rot2, rot);

				// projection matrix (based purely on viewing angles and widget canvas)
				float ftm[16];
				MatProduct(vtm, rot, ftm);

				// instance tm (based purely on mesh instance sliders)

				// here we do only:
				// 2. rotate around z by given angle + random_z
				// 3. rotate by given world's xy axis + random_xy (length is angle)
				MeshPrefs* mp = (MeshPrefs*)GetMeshCookie(active_mesh);

				float itm[16];

				float angle = (float)M_PI / 180 * mp->rotate_locZ_val;
				Rotation(v2, angle, rot2);

				v1[0] = mp->rotate_XY_val[0];
				v1[1] = mp->rotate_XY_val[1];
				v1[2] = 0;

				angle = sqrtf(v1[0]*v1[0] + v1[1]*v1[1]);
				if (angle != 0)
				{
					v1[0]/=angle;
					v1[1]/=angle;
				}

				if (angle>1)
					angle = 1; 

				Rotation(v1, angle * (float)M_PI, rot1);

				MatProduct(rot1, rot2, rot);

				MatProduct(rot, trn, itm);

				// draw!
				RenderContext* rc = &render_context;

				double noon_yaw[2] =
				{
					// zero is behind viewer
					-sin(-lit_yaw * M_PI / 180),
					-cos(-lit_yaw * M_PI / 180),
				};

				double dusk_yaw[3] =
				{
					-noon_yaw[1],
					noon_yaw[0],
					0
				};

				double noon_pos[3] =
				{
					noon_yaw[0] * cos(lit_pitch*M_PI / 180),
					noon_yaw[1] * cos(lit_pitch*M_PI / 180),
					sin(lit_pitch*M_PI / 180)
				};

				double lit_axis[3];

				CrossProduct(dusk_yaw, noon_pos, lit_axis);

				double time_tm[16];
				Rotation(lit_axis, (lit_time - 12)*M_PI / 12, time_tm);

				double lit_pos[4];
				Product(time_tm, noon_pos, lit_pos);

				float lt[4] =
				{
					(float)lit_pos[0],
					(float)lit_pos[1],
					(float)lit_pos[2],
					ambience
				};

				glClearDepth(1.0);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

				glUseProgram(rc->mesh_prg);

				glUniformMatrix4fv(rc->mesh_inst_tm_loc, 1, GL_FALSE, itm);
				glUniformMatrix4fv(rc->mesh_tm_loc, 1, GL_FALSE, ftm);
				glUniform4fv(rc->mesh_lt_loc, 1, lt);
				glUniform1i(rc->mesh_m_tex_loc, 2);
				glUniform1i(rc->mesh_f_tex_loc, 3);
				glUniform1i(rc->mesh_p_tex_loc, 4);

				glBindVertexArray(rc->mesh_vao);

				glBindTextureUnit(2, MyMaterial::tex);
				glBindTextureUnit(3, font[active_font].tex);
				glBindTextureUnit(4, pal_tex);

				glEnable(GL_CULL_FACE);
				glEnable(GL_DEPTH_TEST);
				glDepthFunc(GL_LEQUAL);
				glCullFace(GL_BACK);

				glBindBuffer(GL_ARRAY_BUFFER, rc->mesh_vbo);

				rc->mesh_faces = 0;
				QueryMesh(active_mesh, RenderContext::RenderFace, rc);

				if (rc->mesh_faces)
				{
					// flush!!!
					glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(RenderContext::Face)*rc->mesh_faces,rc->mesh_map);
					glDrawArrays(GL_POINTS, 0, rc->mesh_faces);
					rc->mesh_faces = 0;
				}

				// we should restore !!!!

				glBindBuffer(GL_ARRAY_BUFFER, vbo);

				glBindTextureUnit(2, 0);
				glBindTextureUnit(3, 0);
				glBindTextureUnit(4, 0);

				glBindVertexArray(vao);
				glUseProgram(prg);

				glViewport(vp[0],vp[1],vp[2],vp[3]);
				glScissor(sc[0],sc[1],sc[2],sc[3]);

				if (!cull_face)
					glDisable(GL_CULL_FACE);

				glCullFace(cull_mode);

				if (!depth_test)
					glDisable(GL_DEPTH_TEST);

				glDepthFunc(depth_func);
			}

			bool Widget(const char* label, const ImVec2& size)
			{
				ImGuiWindow* window = ImGui::GetCurrentWindow();
				if (window->SkipItems)
					return false;

				ImGuiContext& g = *GImGui;
				const ImGuiStyle& style = g.Style;
				const ImGuiID id = window->GetID(label);

				ImVec2 pos = window->DC.CursorPos;
				ImVec2 adv(pos.x+size.x,pos.y+size.y);
				
				const ImRect bb(pos, adv);
				rect = bb;

				ImGui::ItemSize(size, style.FramePadding.y);
				if (!ImGui::ItemAdd(bb, id))
					return false;

				ImGui::GetWindowDrawList()->AddCallback(draw_cb, this);
				return true;
			}

			ImRect rect;
		};

		ImGui::Begin("MESH", 0, ImGuiWindowFlags_AlwaysAutoResize);
		{
			static MeshWidget mw;

			// Arrow buttons with Repeater
			float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
			ImGui::PushButtonRepeat(true);
			if (ImGui::ArrowButton("##mesh_prev", ImGuiDir_Left)) 
			{ 
				Mesh* prev = GetPrevMesh(active_mesh);
				if (prev) 
					active_mesh = prev; 
			}

			ImGui::SameLine(0.0f, spacing);

			if (ImGui::ArrowButton("##mesh_next", ImGuiDir_Right)) 
			{ 
				Mesh* next = GetNextMesh(active_mesh);
				if (next) 
					active_mesh = next; 
			}

			ImGui::PopButtonRepeat();
			ImGui::SameLine();
			
			char name[256];
			GetMeshName(active_mesh,name,256);
			ImGui::Text("%s",name);

			mw.Widget("zonk", ImVec2(320,320));
		}
		ImGui::End();

		static int save = 0; // 0-no , 1-save, 2-save_as
		static DirItem** dir_arr = 0;
		static char save_path[4096]="";

		ImGui::Begin("VIEW", 0, ImGuiWindowFlags_AlwaysAutoResize);

		/*
		if (ImGui::Button("DUMP_VT"))
		{
			if (term)
				a3dDumpVT(term);
		}
		*/

		int xywh[4],wh[2];
		a3dGetRect(xywh, wh);
		ImGui::Text("%d,%d,%d,%d %d,%d %s", 
			xywh[0], xywh[1], xywh[2], xywh[3],
			wh[0], wh[1], a3dIsMaximized() ? "MAXIMIZED" : "normal");

		if (ImGui::Button(io.KeyShift ? "DEPALETTIZE" : "PALETTIZE"))
		{
			Palettize(io.KeyShift ? 0 : pal[active_palette].rgb);
		}

		if (!save)
		{
			if (ImGui::Button("SAVE AS"))
			{
				save = 1;

				if (dir_arr)
					FreeDir(dir_arr);
				dir_arr = 0;

				a3dGetCurDir(save_path,4096);
				AllocDir(&dir_arr);
			}

			ImGui::SameLine();

			if (ImGui::Button("LOAD"))
			{
				save = 2;

				if (dir_arr)
					FreeDir(dir_arr);
				dir_arr = 0;

				a3dGetCurDir(save_path,4096);
				AllocDir(&dir_arr);
			}
		}
		else
		{
			if (ImGui::Button("Cancel"))
			{
				save = 0;
				if (dir_arr)
					FreeDir(dir_arr);
				dir_arr = 0;
			}
		}
		

		if (ImGui::Button("FULL"))
		{
			a3dSetRect(0, A3D_WND_FULLSCREEN);
		}
		ImGui::SameLine();
		if (ImGui::Button("NORM"))
		{
			a3dSetRect(0, A3D_WND_NORMAL);
		}
		ImGui::SameLine();
		if (ImGui::Button("PURE"))
		{
			a3dSetRect(0, A3D_WND_FRAMELESS);
		}
		ImGui::SameLine();
		if (ImGui::Button("KEEP"))
		{
			int r[4];
			WndMode mode = a3dGetRect(r, 0);
			a3dSetRect(r, mode);
		}

		static char utf8_buf[1024]="Z\xC3\xB3pa";
		ImGui::InputText("UTF8",utf8_buf,1024);

		if (ImGui::CollapsingHeader("View Control", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::SliderFloat("VIEW PITCH", &rot_pitch, +30.0f, +90.0f);

			ImGui::SliderFloat("VIEW YAW", &rot_yaw, -180.0f, +180.0f); 
			ImGui::SameLine();
			ImGui::Checkbox("Spin", &spin_anim);

			ImGui::SliderFloat("ZOOM", &font_size, 1.0f, 32.0f);
			ImGui::SameLine();
			ImGui::SameLine();
			ImGui::Text("%dx%d", (int)round(io.DisplaySize.x/font_size), (int)round(io.DisplaySize.y / font_size));
		}

		if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Text("PATCHES: %d, DRAWS: %d, CHANGES: %d", render_context.patches, render_context.draws, render_context.changes);
			ImGui::Text("RENDER TIME: %6jd [" /*micro*/"\xc2\xb5"/*utf8*/ "s]", render_context.render_time);
			ImGui::Text("%zu BYTES", GetTerrainBytes(terrain));
		}

		if (ImGui::CollapsingHeader("Light Control", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::SliderFloat("NOON PITCH", &lit_pitch, 0.0f, +90.0f);
			ImGui::SliderFloat("NOON YAW", &lit_yaw, -180.0f, +180.0f);
			ImGui::SliderFloat("LIGHT TIME", &lit_time, 0, 24);
			ImGui::SliderFloat("AMBIENCE", &ambience, 0, 1);

			/*
			ImGui::ColorEdit3("DAWN", dawn_color);
			ImGui::ColorEdit3("NOON", noon_color);
			ImGui::ColorEdit3("DUSK", dusk_color);
			ImGui::ColorEdit3("MIDNIGHT", midnight_color);
			*/
		}

		ImGui::End();
		

		if (save)
		{
			bool save_do = false; // dbl click indicator
			bool show = true;
			ImGui::Begin(save == 1 ? "SAVE" : "LOAD", &show);

			DirItem* cwd = 0;
			ImGui::PushItemWidth(-1);
			if (ImGui::InputText("###path",save_path,4096,ImGuiInputTextFlags_EnterReturnsTrue))
			{
				if (save == 1)
				{
					// SAVE to save_path, warn if file exist?
					FILE* f = fopen(save_path,"wb");
					if (f)
					{
						if (SaveTerrain(terrain,f))
						{
							// save mats
							for (int i=0; i<256; i++)
								fwrite(mat[i].shade,1,sizeof(Cell)*4*16,f);

							// close save dialog
							save = 0;
							if (dir_arr)
								FreeDir(dir_arr);
							dir_arr = 0;
						}

						fclose(f);
					}
				}
				else
				if (save == 2)
				{
					// load

					URDO_Purge();
					DeleteTerrain(terrain);
					terrain = 0;

					FILE* f = fopen(save_path,"rb");
					if (f)
					{
						terrain = LoadTerrain(f);

						if (terrain)
						{
							for (int i=0; i<256; i++)
							{
								if ( fread(mat[i].shade,1,sizeof(Cell)*4*16,f) != sizeof(Cell)*4*16 )
									break;

								mat[i].Update();
							}
						}

						fclose(f);
					}

					if (!terrain)
					{
						terrain = CreateTerrain();
					}

					// close save dialog
					save = 0;
					if (dir_arr)
						FreeDir(dir_arr);
					dir_arr = 0;					
				}
			}

			if (save && ImGui::ListBoxHeader("###dir", ImVec2(-1, -ImGui::GetItemsLineHeightWithSpacing()) ))
			{
				// fill from dir_arr
				DirItem** di = dir_arr;
				while (*di)
				{
					if ((*di)->item == A3D_DIRECTORY)
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,0,1));
						
					if (ImGui::Selectable((*di)->name,false, ImGuiSelectableFlags_AllowDoubleClick))
					{
						if ((*di)->item == A3D_FILE)
						{
							// just copy its path to editbox
							char cd[4096];
							a3dGetCurDir(cd,4096);
							snprintf(save_path,4096,"%s%s",cd,(*di)->name);

							if (ImGui::IsMouseDoubleClicked(0))
								save_do = true;								
						}
						else
						{
							// change current directory and rescan after 
							cwd = *di;
						}
					}
					if ((*di)->item == A3D_DIRECTORY)
						ImGui::PopStyleColor();
					di++;
				}
				ImGui::ListBoxFooter();
			}

			

			if (save && (ImGui::Button(save == 1 ? "SAVE" : "LOAD") || save_do))
			{
				if (save == 1)
				{
					// SAVE to save_path, warn if file exist?
					FILE* f = fopen(save_path,"wb");
					if (f)
					{
						if (SaveTerrain(terrain,f))
						{
							// save mats
							for (int i=0; i<256; i++)
								fwrite(mat[i].shade,1,sizeof(Cell)*4*16,f);

							// close save dialog
							save = 0;
							if (dir_arr)
								FreeDir(dir_arr);
							dir_arr = 0;
						}

						fclose(f);
					}
				}
				else
				if (save == 2)
				{
					// load

					URDO_Purge();
					DeleteTerrain(terrain);
					terrain = 0;

					FILE* f = fopen(save_path,"rb");
					if (f)
					{
						terrain = LoadTerrain(f);

						if (terrain)
						{
							for (int i=0; i<256; i++)
							{
								if ( fread(mat[i].shade,1,sizeof(Cell)*4*16,f) != sizeof(Cell)*4*16 )
									break;	
								mat[i].Update();
							}
						}

						fclose(f);
					}

					if (!terrain)
					{
						terrain = CreateTerrain();
					}

					// close save dialog
					save = 0;
					if (dir_arr)
						FreeDir(dir_arr);
					dir_arr = 0;					
				}
			}	

			ImGui::SameLine();
			if (save && ImGui::Button("CANCEL"))
			{
				// close save dialog
				save = 0;
				if (dir_arr)
					FreeDir(dir_arr);
				dir_arr = 0;
			}				

			if (save && cwd && show)
			{
				a3dSetCurDir(cwd->name);
				a3dGetCurDir(save_path,4096);
				if (dir_arr)
					FreeDir(dir_arr);
				dir_arr = 0;

				a3dGetCurDir(save_path,4096);
				AllocDir(&dir_arr);
			}

			ImGui::End();

			if (!show)
			{
				if (dir_arr)
					FreeDir(dir_arr);
				dir_arr = 0;

				save = 0;
			}
		}


		/// end of window?
		ImGui::Begin("EDIT", 0, ImGuiWindowFlags_AlwaysAutoResize);

		if (ImGui::CollapsingHeader("Undo / Redo", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (!URDO_CanUndo())
			{
				ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
				ImGui::Button("<<");
				ImGui::SameLine();
				ImGui::Button("<");
				ImGui::PopStyleVar();
				ImGui::PopItemFlag();
			}
			else
			{
				if (ImGui::Button("<<") || ImGui::IsItemActive() && io.MouseDownDuration[0] > .25f)
					URDO_Undo(0);
				ImGui::SameLine();
				if (ImGui::Button("<") || ImGui::IsItemActive() && io.MouseDownDuration[0] > .25f)
					URDO_Undo(1);
			}
			ImGui::SameLine();
			if (!URDO_CanRedo())
			{
				ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
				ImGui::Button(">");
				ImGui::SameLine();
				ImGui::Button(">>");
				ImGui::PopStyleVar();
				ImGui::PopItemFlag();
			}
			else
			{
				if (ImGui::Button(">") || ImGui::IsItemActive() && io.MouseDownDuration[0] > .25f)
					URDO_Redo(1);
				ImGui::SameLine();
				if (ImGui::Button(">>") || ImGui::IsItemActive() && io.MouseDownDuration[0] > .25f)
					URDO_Redo(0);
			}
			ImGui::SameLine();
			if (!URDO_CanRedo() && !URDO_CanUndo())
			{
				ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
				ImGui::Button("PURGE");
				ImGui::PopStyleVar();
				ImGui::PopItemFlag();
			}
			else
				if (ImGui::Button("PURGE"))
					URDO_Purge();
			ImGui::SameLine();
			ImGui::Text("%zu BYTES", URDO_Bytes());
		}

		if (ImGui::CollapsingHeader("Brush", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
			if (ImGui::BeginTabBar("MyTabBar", tab_bar_flags))
			{
				bool pushed = false;

				if (edit_mode != 0)
				{
					pushed = true;
					ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
				}
				if (ImGui::BeginTabItem("SCULPT"))
				{
					edit_mode = 0;
					ImGui::Text("Sculpting modifies terrain height map \n ");

					const char* mode = "";

					if (!painting && io.KeyCtrl && io.KeyShift)
					{
						mode = "HEIGHT PROBE";
					}
					else
					if (!painting && io.KeyCtrl)
						mode = "DIAGONAL FLIP";
					else
					{
						if (io.KeyShift)
							mode = br_alpha >= 0 ? "BLURRING" : "SHARPENING";
						else
							mode = br_alpha >= 0 ? "ASCENT" : "DESCENT";
					}

					ImGui::Text("MODE (shift/ctrl): %s", mode);
					ImGui::SliderFloat("BRUSH RADIUS", &br_radius, 5.f, 100.f);
					ImGui::SliderFloat("BRUSH ALPHA", &br_alpha, -0.5f, +0.5f);


					ImGui::Checkbox("BRUSH HEIGHT LIMIT",&br_limit);
					ImGui::SameLine();

					// Arrow buttons with Repeater
					float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
					ImGui::PushButtonRepeat(true);
					if (ImGui::ArrowButton("##probe_left", ImGuiDir_Left)) { if (probe_z>0) probe_z-=1; }
					ImGui::SameLine(0.0f, spacing);
					if (ImGui::ArrowButton("##probe_right", ImGuiDir_Right)) { if (probe_z<0xffff) probe_z+=1; }
					ImGui::PopButtonRepeat();
					ImGui::SameLine();
					ImGui::Text("%d", probe_z);
					ImGui::Text("%s", "ctrl+shift to probe");

					// ImGui::SliderFloat("BRUSH HEIGHT", &probe_z, 0.0f, 65535.0f);

					ImGui::EndTabItem();
				}
				if (pushed)
				{
					pushed = false;
					ImGui::PopStyleVar();
				}

				if (edit_mode != 1)
				{
					pushed = true;
					ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
				}
				if (ImGui::BeginTabItem("MAT-id"))
				{
					edit_mode = 1;
					ImGui::Text("Material channel selects which material \ndefinition should be used (0-255)");

					const char* mode = "";

					// painting with shift (and enabled z-limit)
					// could reverse painting above with below ....

					if (!painting && io.KeyCtrl && io.KeyShift)
					{
						mode = "HEIGHT PROBE";
					}
					else
					if (!painting && io.KeyCtrl)
						mode = "MAT-id PROBE";
					else
					{
						if (br_limit)
						{
							if (io.KeyShift)
								mode = "PAINT BELOW";
							else
								mode = "PAINT ABOVE";
						}
						else
							mode = "PAINT";
					}

					ImGui::Text("MODE (shift/ctrl): %s", mode);
					ImGui::SliderFloat("BRUSH DIAMETER", &br_radius, 1.f, 100.f);

					float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
					ImGui::PushButtonRepeat(true);
					if (ImGui::ArrowButton("##matid_left", ImGuiDir_Left)) { if (active_material>0) active_material-=1; }
					ImGui::SameLine(0.0f, spacing);
					if (ImGui::ArrowButton("##matid_right", ImGuiDir_Right)) { if (active_material<0xff) active_material+=1; }
					ImGui::PopButtonRepeat();
					ImGui::SameLine();
					ImGui::Text("MAT-id 0x%02X (%d)", active_material, active_material);
					ImGui::SameLine();
					ImGui::Text("%s", "ctrl to probe");


					ImGui::Checkbox("BRUSH HEIGHT LIMIT",&br_limit);
					ImGui::SameLine();

					// Arrow buttons with Repeater
					ImGui::PushButtonRepeat(true);
					if (ImGui::ArrowButton("##probe_left", ImGuiDir_Left)) { if (probe_z>0) probe_z-=1; }
					ImGui::SameLine(0.0f, spacing);
					if (ImGui::ArrowButton("##probe_right", ImGuiDir_Right)) { if (probe_z<0xffff) probe_z+=1; }
					ImGui::PopButtonRepeat();
					ImGui::SameLine();
					ImGui::Text("%d", probe_z);
					ImGui::Text("%s", "ctrl+shift to probe");
					ImGui::Text("%s", "press shift to paint below limit");

					ImGui::EndTabItem();
				}
				if (pushed)
				{
					pushed = false;
					ImGui::PopStyleVar();
				}

				static bool add_verts = false;
				static bool build_poly = false;

				if (active_mesh && edit_mode != 2)
				{
					pushed = true;
					ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);

					add_verts = false;
					build_poly = false;
				}
				if (active_mesh && ImGui::BeginTabItem("MESH"))
				{
					edit_mode = 2;

					// when putting new instance we do:
					// 1. pretranslate (to have 0 in rot/scale center)
					// 2. scale by constant_xyz * random_xyz 
					// 2. rotate around z by given angle + random_z
					// 3. rotate by given world's xy axis + random_xy (length is angle)
					// 4. rotate toward terrain normal by given weight
					// 5. post translate by constant xyz + random xyz

					extern int bsp_insts, bsp_nodes, bsp_tests;
					ImGui::Text("INSTS:%d, NODES:%d, TESTS:%d", bsp_insts, bsp_nodes, bsp_tests);


					MeshPrefs* mp = (MeshPrefs*)GetMeshCookie(active_mesh);

					//ImGui::SliderFloat3("PreTranslate", mp->pre_trans, -1, +1);
					//ImGui::Separator();
					ImGui::SliderFloat3("ScaleValue", mp->scale_val, -5, +5); // pow of 2
					ImGui::SliderFloat3("ScaleRand", mp->scale_rnd, 0, 1);  // pow of 2
					ImGui::Separator();
					ImGui::SliderFloat("RotateLocZValue", &mp->rotate_locZ_val, -180, 180);
					ImGui::SliderFloat("RotateLocZRand", &mp->rotate_locZ_rnd, 0, 1);
					ImGui::Separator();
					ImGui::SliderFloat2("RotateXYValue", mp->rotate_XY_val, -180, +180);
					ImGui::SliderFloat2("RotateXYRand", mp->rotate_XY_rnd, 0, 1);
					ImGui::Separator();
					//ImGui::SliderFloat3("TranslateValue", mp->translate_val, -1, +1);
					//ImGui::SliderFloat3("TranslateRand", mp->translate_rnd, 0, 1);
					ImGui::Separator();
					ImGui::SliderFloat("RotateAlign", &mp->rotate_align, 0, 1);

					ImGui::EndTabItem();
				}
				if (active_mesh && pushed)
				{
					pushed = false;
					ImGui::PopStyleVar();
				}

				/*
				if (edit_mode != 2)
				{
					pushed = true;
					ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
				}
				if (ImGui::BeginTabItem("sh-MODE"))
				{
					edit_mode = 2;
					ImGui::Text("Shade mode channel specifies how lighting \naffects shading ramp (0-3)");
					ImGui::EndTabItem();
				}
				if (pushed)
				{
					pushed = false;
					ImGui::PopStyleVar();
				}

				if (edit_mode != 3)
				{
					pushed = true;
					ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
				}
				if (ImGui::BeginTabItem("sh-RAMP"))
				{
					edit_mode = 3;
					ImGui::Text("Shade ramp channel selects a cell \nhorizontaly from a material ramps (0-15)");
					ImGui::EndTabItem();
				}
				if (pushed)
				{
					pushed = false;
					ImGui::PopStyleVar();
				}

				if (edit_mode != 4)
				{
					pushed = true;
					ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
				}
				if (ImGui::BeginTabItem("ELEV"))
				{
					edit_mode = 4;
					ImGui::Text("Elevation bits are used to choose ramps \nvertically from material by bit difference");
					ImGui::EndTabItem();
				}
				if (pushed)
				{
					pushed = false;
					ImGui::PopStyleVar();
				}
				*/

				ImGui::EndTabBar();
			}
		}

		ImGui::End();
		/// end of window?
		ImGui::Begin("FONT", 0, ImGuiWindowFlags_AlwaysAutoResize);

		// fonts related stuff
		float font_width = (float)font[active_font].width;
		float font_height = (float)font[active_font].height;
		if (font_width<256)
		{
			font_width = 256;
			font_height *= 256.0f / font[active_font].width;
		}

		int glyph_w = font[active_font].width / 16;
		int glyph_h = font[active_font].height / 16;
		float glyph_x = (active_glyph & 0xf) * glyph_w / (float)font[active_font].width;
		float glyph_y = (active_glyph >> 4) * glyph_h / (float)font[active_font].height;
		float texel_w = 1.0f / font[active_font].width;
		float texel_h = 1.0f / font[active_font].height;
		float but_w = 13 + 48.0f / (font_width / 16);	

		float but16_w = font_width / 16;
		float but16_h = font_height / 16;

		if (fonts_loaded && ImGui::CollapsingHeader("Fonts", ImGuiTreeNodeFlags_DefaultOpen))
		{
			float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
			ImGui::PushButtonRepeat(true);
			if (ImGui::ArrowButton("##fnt_left", ImGuiDir_Left)) { if (active_font > 0) active_font--; }
			ImGui::SameLine(0.0f, spacing);
			if (ImGui::ArrowButton("##fnt_right", ImGuiDir_Right)) { if (active_font < fonts_loaded-1) active_font++; }
			ImGui::PopButtonRepeat();
			ImGui::SameLine();
			ImGui::Text("0x%02X (%d)", active_font, active_font); // path?

			ImGui::Text("CELL SIZE: %dx%d px", font[active_font].width/16, font[active_font].height/16);
			//ImGui::Image((void*)(intptr_t)font.tex, ImVec2(font.width,font.height), ImVec2(0,1), ImVec2(1,0));

			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
			ImVec4 tint_normal(1, 1, 1, 0.33f);
			ImVec4 tint_onedim(1, 1, 1, 0.50f);
			ImVec4 tint_active(1, 1, 1, 1.00f);
			for (int y = 0; y < 16; y++)
			{
				for (int x = 0; x < 16; x++)
				{
					ImVec4* tint = &tint_normal;

					bool pushed = false;
					if (x + y*16 == active_glyph)
					{
						ImVec4 hi = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
						ImGui::PushStyleColor(ImGuiCol_Button,hi);
						tint = &tint_active;
						pushed = true;
					}
					else
					if (x == (active_glyph & 0xf) || y == (active_glyph>>4))
						tint = &tint_onedim;

					ImGui::PushID(x + y * 16);
					if (ImGui::ImageButton((void*)(intptr_t)font[active_font].tex, 
						//ImVec2(font[active_font].width / 16.f, font[active_font].height / 16.f),
						ImVec2(font_width / 16.f, font_height / 16.f),
						ImVec2(x / 16.0f, y / 16.0f), ImVec2((x + 1) / 16.0f, (y + 1)/ 16.0f), 1, ImVec4(0, 0, 0, 0), *tint))
					{
						active_glyph = x + y * 16;
					}
					ImGui::PopID();

					if (pushed)
						ImGui::PopStyleColor();

					if (x<15)
						ImGui::SameLine();
				}
			}
			ImGui::PopStyleVar();
		}

		if (fonts_loaded && ImGui::CollapsingHeader("Character", ImGuiTreeNodeFlags_DefaultOpen))
		{
			float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
			ImGui::PushButtonRepeat(true);
			if (ImGui::ArrowButton("##chr_left", ImGuiDir_Left)) { if (active_glyph > 0) active_glyph--; }
			ImGui::SameLine(0.0f, spacing);
			if (ImGui::ArrowButton("##chr_right", ImGuiDir_Right)) { if (active_glyph < 0xff) active_glyph++; }
			ImGui::PopButtonRepeat();
			ImGui::SameLine();
			ImGui::Text("0x%02X (%d)", active_glyph, active_glyph);


			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

			for (int y = 0; y < glyph_h ; y++)
			{
				for (int x = 0; x < glyph_w; x++)
				{
					ImGui::PushID(x + y * glyph_w + 256);
					if (ImGui::ImageButton((void*)(intptr_t)font[active_font].tex, ImVec2(but_w, but_w),
						ImVec2(glyph_x + x*texel_w, glyph_y + y*texel_h), ImVec2(glyph_x + (x+1)*texel_w, glyph_y + (y+1)*texel_h), 
						1, ImVec4(0,0,0,.5f), ImVec4(1,1,1,.5)))
					{
						int u = x + glyph_w*(active_glyph&0xF);
						int v = y + glyph_h*(active_glyph>>4);
						// tick that pixel
						uint8_t p = font[active_font].GetTexel(u,v);
						p ^=0xFF;
						font[active_font].SetTexel(u,v,p);
					}
					ImGui::PopID();

					if (x < glyph_w-1)
						ImGui::SameLine();
				}
			}

			ImGui::PopStyleVar();
		}

		ImGui::End();
		/// end of window?
		ImGui::Begin("SKIN", 0, ImGuiWindowFlags_AlwaysAutoResize);

		if (ImGui::CollapsingHeader("Palettes", ImGuiTreeNodeFlags_DefaultOpen))
		{
			float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
			ImGui::PushButtonRepeat(true);
			if (ImGui::ArrowButton("##pal_left", ImGuiDir_Left)) { if (active_palette > 0) active_palette--; }
			ImGui::SameLine(0.0f, spacing);
			if (ImGui::ArrowButton("##pal_right", ImGuiDir_Right)) { if (active_palette < 0xff) active_palette++; }
			ImGui::PopButtonRepeat();
			ImGui::SameLine();
			ImGui::Text("0x%02X (%d)", active_palette, active_palette);

			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

			for (int y = 0; y < 16; y++)
			{
				for (int x = 0; x < 16; x++)
				{
					ImVec4 tint(
						pal[active_palette].rgb[3 * (x + 16 * y) + 0] / 255.0f, 
						pal[active_palette].rgb[3 * (x + 16 * y) + 1] / 255.0f,
						pal[active_palette].rgb[3 * (x + 16 * y) + 2] / 255.0f,
						1.0
					);

#if 0
					ImGui::PushID(x + y * 16 + 256 + glyph_w * glyph_h);
					if (ImGui::ImageButton(0/*samples black!*/,
						//ImVec2(glyph_w, glyph_w), 
						ImVec2(but16_w, but16_h),
						ImVec2(0,0), ImVec2(1,1), 1, tint, ImVec4(0,0,0,0)))
					{
						// select that color
					}
					ImGui::PopID();
#endif
					ImGui::PushID(x + y * 16 + 256 + glyph_w * glyph_h);

					if (ImGui::ColorEdit3("", (float*)&tint, ImGuiColorEditFlags_NoInputs, ImVec2(but16_w + 2, but16_h + 2)))
					{
						pal[active_palette].rgb[3 * (x + 16 * y) + 0] = (int)round(tint.x * 255);
						pal[active_palette].rgb[3 * (x + 16 * y) + 1] = (int)round(tint.y * 255);
						pal[active_palette].rgb[3 * (x + 16 * y) + 2] = (int)round(tint.z * 255);
					}
					
					ImGui::PopID();

					if (x < 15)
						ImGui::SameLine();
				}
			}

			ImGui::PopStyleVar();
		}

		if (fonts_loaded && ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen))
		{
			float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
			ImGui::PushButtonRepeat(true);
			if (ImGui::ArrowButton("##mat_left", ImGuiDir_Left)) { if (active_material > 0) active_material--; }
			ImGui::SameLine(0.0f, spacing);
			if (ImGui::ArrowButton("##mat_right", ImGuiDir_Right)) { if (active_material < 0xff) active_material++; }
			ImGui::PopButtonRepeat();
			ImGui::SameLine();
			ImGui::Text("0x%02X (%d)", active_material, active_material);

			static bool paint_mat_glyph = true;
			static bool paint_mat_foreground = true;
			static bool paint_mat_background = true;

			static float paint_mat_fg[3] = { .2f, .3f, .4f };
			static float paint_mat_bg[3] = { .2f, .2f, .1f };

			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

			for (int y = 0; y < 3; y++)
			{
				for (int x = 0; x < 16; x++)
				{
					float glyph_x = (mat[active_material].shade[y][x].gl & 0xF) / 16.0f;
					float glyph_y = (mat[active_material].shade[y][x].gl >> 4) / 16.0f;

					uint8_t* bg = mat[active_material].shade[y][x].bg;
					uint8_t* fg = mat[active_material].shade[y][x].fg;

					ImGui::PushID(x + y * 16 + 512 + glyph_w * glyph_h);
					if (ImGui::ImageButton((void*)(intptr_t)font[active_font].tex, 
						// ImVec2(glyph_w, glyph_h),
						ImVec2(but16_w, but16_h),
						ImVec2(glyph_x, glyph_y), ImVec2(glyph_x + 1 / 16.0f, glyph_y + 1 / 16.0f), 
						1, ImVec4(bg[0] / 255.f, bg[1] / 255.f, bg[2] / 255.f, 1), 
						ImVec4(fg[0] / 255.f, fg[1] / 255.f, fg[2] / 255.f, 1)))
					{
						if (paint_mat_glyph)
							mat[active_material].shade[y][x].gl = active_glyph;

						if (paint_mat_foreground)
						{
							fg[0] = (int)round(paint_mat_fg[0] * 255);
							fg[1] = (int)round(paint_mat_fg[1] * 255);
							fg[2] = (int)round(paint_mat_fg[2] * 255);
						}

						if (paint_mat_background)
						{
							bg[0] = (int)round(paint_mat_bg[0] * 255);
							bg[1] = (int)round(paint_mat_bg[1] * 255);
							bg[2] = (int)round(paint_mat_bg[2] * 255);
						}

						mat[active_material].Update();
					}

					if (ImGui::IsItemClicked(1) && !io.MouseDown[0])
					{
						// this is cell probe
						int a = 0;
						if (paint_mat_foreground)
						{
							paint_mat_fg[0] = fg[0] / 255.0f;
							paint_mat_fg[1] = fg[1] / 255.0f;
							paint_mat_fg[2] = fg[2] / 255.0f;
						}
						if (paint_mat_background)
						{
							paint_mat_bg[0] = bg[0] / 255.0f;
							paint_mat_bg[1] = bg[1] / 255.0f;
							paint_mat_bg[2] = bg[2] / 255.0f;
						}
						if (paint_mat_glyph)
						{
							active_glyph = mat[active_material].shade[y][x].gl;
						}
					}

					if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
					{
						int cookie = x + 16 * y;
						ImGui::SetDragDropPayload("DND_MAT_RAMPING", &cookie, sizeof(int));
						ImGui::Text("RAMPING");
						ImGui::EndDragDropSource();
					}

					if (ImGui::BeginDragDropTarget())
					{
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_MAT_RAMPING"))
						{
							IM_ASSERT(payload->DataSize == sizeof(int));
							int cookie = *(const int*)payload->Data;

							int x1 = cookie & 0xF;
							int y1 = cookie >> 4;
							int x2 = x;
							int y2 = y;

							if (y1 > y2)
							{
								int s = y1;
								y1 = y2;
								y2 = s;
							}

							if (x1 > x2)
							{
								int s = x1;
								x1 = x2;
								x2 = s;
							}

							// action!
							for (int dy = y1; dy <= y2; dy++)
							{
								// read endpoints
								Cell c1 = mat[active_material].shade[dy][x1];
								Cell c2 = mat[active_material].shade[dy][x2];

								for (int dx = x1 + 1; dx < x2; dx++)
								{
									Cell* c = &(mat[active_material].shade[dy][dx]);
									float w = (float)(dx - x1) / (float)(x2 - x1);
									// interpolate
									if (paint_mat_foreground)
									{
										c->fg[0] = (int)roundf(c1.fg[0] * (1 - w) + c2.fg[0] * w);
										c->fg[1] = (int)roundf(c1.fg[1] * (1 - w) + c2.fg[1] * w);
										c->fg[2] = (int)roundf(c1.fg[2] * (1 - w) + c2.fg[2] * w);
									}
									if (paint_mat_background)
									{
										c->bg[0] = (int)roundf(c1.bg[0] * (1 - w) + c2.bg[0] * w);
										c->bg[1] = (int)roundf(c1.bg[1] * (1 - w) + c2.bg[1] * w);
										c->bg[2] = (int)roundf(c1.bg[2] * (1 - w) + c2.bg[2] * w);
									}
									if (paint_mat_glyph)
									{
										if (dx - x1 < x2 - dx)
											c->gl = c1.gl;
										else
											c->gl = c2.gl;
									}
								}
							}

							mat[active_material].Update();
						}
						ImGui::EndDragDropTarget();
					}

					ImGui::PopID();

					if (x < 15)
						ImGui::SameLine();
				}
			}

			ImGui::PopStyleVar();

			ImGui::Separator();

			ImGui::Checkbox("Glyph", &paint_mat_glyph); ImGui::SameLine(); ImGui::Text("0x%02X (%d)", active_glyph, active_glyph);
			ImGui::Checkbox("Foreground", &paint_mat_foreground); ImGui::SameLine(); ImGui::ColorEdit3("###FG", paint_mat_fg);
			ImGui::Checkbox("Background", &paint_mat_background); ImGui::SameLine(); ImGui::ColorEdit3("###BG", paint_mat_bg);
		}

		ImGui::End();

		static bool show_demo_window = true;
		static bool show_another_window = false;

		// 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
		//if (show_demo_window)
		//	ImGui::ShowDemoWindow(&show_demo_window);

		/*

		// 2. Show a simple window that we create ourselves. We use a Begin/End pair to created a named window.
		{
			static float f = 0.0f;
			static int counter = 0;

			ImGui::Begin("Hello, world!");                          // Create a window called "Hello, world!" and append into it.

			ImGui::Text("This is some useful text.");               // Display some text (you can use a format strings too)
			ImGui::Checkbox("Demo Window", &show_demo_window);      // Edit bools storing our window open/close state
			ImGui::Checkbox("Another Window", &show_another_window);

			ImGui::SliderFloat("float", &f, 0.0f, 1.0f);            // Edit 1 float using a slider from 0.0f to 1.0f
			ImGui::ColorEdit3("clear color", (float*)&clear_color); // Edit 3 floats representing a color

			if (ImGui::Button("Button"))                            // Buttons return true when clicked (most widgets return true when edited/activated)
				counter++;
			ImGui::SameLine();
			ImGui::Text("counter = %d", counter);

			ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

			ImGui::Text("PATCHES: %d, DRAWS: %d, CHANGES: %d", render_context.patches, render_context.draws, render_context.changes);

			ImGui::End();
		}
		*/

		// 3. Show another simple window.
		/*
		if (show_another_window)
		{
			ImGui::Begin("Another Window", &show_another_window);   // Pass a pointer to our bool variable (the window will have a closing button that will clear the bool when clicked)
			ImGui::Text("Hello from another window!");
			if (ImGui::Button("Close Me"))
				show_another_window = false;
			ImGui::End();
		}
		*/

//		if (pFont)
//			ImGui::PopFont();
	}

	ImGui::Render();

	glViewport(0, 0, (GLsizei)io.DisplaySize.x, (GLsizei)io.DisplaySize.y);

	glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
	glClearDepth(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	RenderContext* rc = &render_context;
	double tm[16];

	// currently we're assuming: 1 visual cell = 1 font_size

	double z_scale = 1.0 / HEIGHT_SCALE; // this is a constant, (what fraction of font_size is produced by +1 height_map)

	if (!io.MouseDown[0])
	{
		diag_flipped = false;
		inst_added = false;
	}

	if (!io.MouseDown[1])
	{
		spinning = 0;
	}

	if (!io.MouseDown[2])
	{
		panning = 0;
	}

	if (!io.WantCaptureMouse)
	{
		if (zoom_wheel)
		{
			font_size *= powf(1.1f, zoom_wheel);
			zoom_wheel = 0;
		}

		if (spinning)
		{
			double mdx = spinning_x - round(io.MousePos.x);
			double mdy = -(spinning_y - round(io.MousePos.y));

			rot_yaw += (float)(mdx * 0.1);
			if (rot_yaw < -180)
				rot_yaw += 360;
			if (rot_yaw > 180)
				rot_yaw -= 360;

			rot_pitch += (float)(mdy * 0.1);
			if (rot_pitch > 90)
				rot_pitch = 90;
			if (rot_pitch < 30)
				rot_pitch = 30;


			spinning_x = (int)roundf(io.MousePos.x);
			spinning_y = (int)roundf(io.MousePos.y);
		}
		else
		if (io.MouseDown[1])
		{
			spinning = 1;
			spinning_x = (int)roundf(io.MousePos.x);
			spinning_y = (int)roundf(io.MousePos.y);
		}
	}
	else
	{
		zoom_wheel = 0;
	}
	

	double rx = 0.5 * io.DisplaySize.x / font_size;
	double ry = 0.5 * io.DisplaySize.y / font_size;

	double pitch = rot_pitch * (M_PI / 180);
	double yaw = rot_yaw * (M_PI / 180);


	if (spin_anim)
	{
		rot_yaw += 0.1f;
		if (rot_yaw > 180)
			rot_yaw -= 360;
	}

	if (!io.WantCaptureMouse)
	{
		if (panning)
		{
			double mdx = panning_x - round(io.MousePos.x);
			double mdy = -(panning_y - round(io.MousePos.y)) / sin(pitch);
			pos_x = (float)(panning_dx + (mdx*cos(yaw) - mdy * sin(yaw)) / font_size);
			pos_y = (float)(panning_dy + (mdx*sin(yaw) + mdy * cos(yaw)) / font_size);

			panning_x = (int)roundf(io.MousePos.x);
			panning_y = (int)roundf(io.MousePos.y);

			panning_dx = pos_x;
			panning_dy = pos_y;
		}
		else
		if (io.MouseDown[2])
		{
			panning = 1;
			panning_x = (int)roundf(io.MousePos.x);
			panning_y = (int)roundf(io.MousePos.y);
			panning_dx = pos_x;
			panning_dy = pos_y;
		}
	}

	tm[0] = +cos(yaw)/rx;
	tm[1] = -sin(yaw)*sin(pitch)/ry;
	tm[2] = 0;
	tm[3] = 0;
	tm[4] = +sin(yaw)/rx;
	tm[5] = +cos(yaw)*sin(pitch)/ry;
	tm[6] = 0;
	tm[7] = 0;
	tm[8] = 0;
	tm[9] = +cos(pitch)*z_scale/ry;
	tm[10] = +2./0xffff;
	tm[11] = 0;
	tm[12] = -(pos_x * tm[0] + pos_y * tm[4] + pos_z * tm[8]);
	tm[13] = -(pos_x * tm[1] + pos_y * tm[5] + pos_z * tm[9]);
	tm[14] = -1.0;
	tm[15] = 1.0;

	float br_xyra[4] = { 0,0, br_radius, 0 };
	float br_quad[3] = { 0,0,0 };
	float br_probe[3] = { (float)probe_z, 1.0f, br_limit ? br_alpha : 0.0f };

	bool create_preview = false;
	int create_preview_px = 0;
	int create_preview_py = 0;

	double inst_tm[16];
	Mesh* inst_preview = 0;

	if (!io.WantCaptureMouse && mouse_in)
	{
		if (painting || creating)
		{
			if (creating)
			{
				double mdx = painting_x - round(io.MousePos.x);
				double mdy = -(painting_y - round(io.MousePos.y)) / sin(pitch);
				double dx = -(mdx*cos(yaw) - mdy * sin(yaw)) / font_size;
				double dy = -(mdx*sin(yaw) + mdy * cos(yaw)) / font_size;
				double x = painting_dx + dx;
				double y = painting_dy + dy;

				int px = (int)floor(x / VISUAL_CELLS);
				int py = (int)floor(y / VISUAL_CELLS);

				if (creating < 0)
				{
					// LOCATE & DELETE PATCH IF EXIST
					Patch* p = GetTerrainPatch(terrain, px, py);
					if (p)
						URDO_Delete(terrain, p);
				}
				else
				{
					// IF NO PATCH THERE, CREATE ONE
					Patch* p = GetTerrainPatch(terrain, px, py);
					if (!p)
						p = URDO_Create(terrain, px, py, probe_z);
				}

				painting_dx = x;
				painting_dy = y;
				painting_x = (int)round(io.MousePos.x);
				painting_y = (int)round(io.MousePos.y);

				if (!io.MouseDown[0])
				{
					creating = 0;
					URDO_Close();
				}
			}
			else // painting
			{
				if (painting == 1)
				{
					//DRAG and/or DROP
					double mdx = painting_x - round(io.MousePos.x);
					double mdy = -(painting_y - round(io.MousePos.y)) / sin(pitch);
					double dx = -(mdx*cos(yaw) - mdy * sin(yaw)) / font_size;
					double dy = -(mdx*sin(yaw) + mdy * cos(yaw)) / font_size;
					double x = painting_dx + dx;
					double y = painting_dy + dy;

					double dist = paint_dist + sqrt(dx*dx + dy * dy);

					int i = 0;
					float alpha = br_alpha;
					br_alpha *= STAMP_A;
					while (1)
					{
						double w = ((i + 1) * br_radius * STAMP_R - paint_dist) / (dist - paint_dist);

						if (w >= 1)
							break;

						double sx = painting_dx + w * dx;
						double sy = painting_dy + w * dy;

						Stamp(sx, sy);

						i++;
					}
					br_alpha = alpha;

					paint_dist = dist - i * br_radius * STAMP_R;
					painting_dx = x;
					painting_dy = y;
					painting_x = (int)round(io.MousePos.x);
					painting_y = (int)round(io.MousePos.y);

					br_xyra[0] = (float)x;
					br_xyra[1] = (float)y;

					if (!io.MouseDown[0])
					{
						// DROP
						float alpha = br_alpha;
						br_alpha *= (float)pow(paint_dist / (br_radius * STAMP_R) * STAMP_A, 2.0);
						Stamp(x, y);
						br_alpha = alpha;
						br_xyra[3] = 0;
						painting = 0;
						URDO_Close();
					}
					else
						br_xyra[3] = (float)pow(paint_dist / (br_radius * STAMP_R) * STAMP_A, 2.0) * br_alpha;
				}
				else
				if (painting == 2)
				{
					double mdx = painting_x - round(io.MousePos.x);
					double mdy = -(painting_y - round(io.MousePos.y)) / sin(pitch);

					if (mdx || mdy)
					{
						double dx = -(mdx*cos(yaw) - mdy * sin(yaw)) / font_size;
						double dy = -(mdx*sin(yaw) + mdy * cos(yaw)) / font_size;
						double x = painting_dx + dx;
						double y = painting_dy + dy;

						double hit[2] = { x,y };
						MatIDStamp stamp;
						stamp.r = br_radius * 0.5;
						stamp.hit = hit;
						stamp.z = br_probe[0];
						stamp.z_lim = br_limit ? (io.KeyShift ? -1 : 1) : 0;

						URDO_Open();
						QueryTerrain(terrain, hit[0], hit[1], br_radius * 0.501, 0x00, MatIDStamp::SetMatCB, &stamp);
						URDO_Close();

						painting_dx = x;
						painting_dy = y;
						painting_x = (int)round(io.MousePos.x);
						painting_y = (int)round(io.MousePos.y);
					}

					if (!io.MouseDown[0])
					{
						// DROP
						painting = 0;
						URDO_Close();
					}
				}
			}
		}
		else
		{
			// HOVER preview
			// all coords in world space!
			double itm[16];
			Invert(tm, itm);

			double ray_p[4];
			double ray_v[4];

			// mouse ray
			double clip_mouse[4] =
			{
				2.0 * io.MousePos.x / io.DisplaySize.x - 1.0,
				1.0 - 2.0 * io.MousePos.y / io.DisplaySize.y,
				-1.1, // bit under floor
				1
			};

			Product(itm, clip_mouse, ray_p);

			clip_mouse[2] = -1.2; // bit under bit under floor

			Product(itm, clip_mouse, ray_v);

			ray_v[0] -= ray_p[0];
			ray_v[1] -= ray_p[1];
			ray_v[2] -= ray_p[2];

			double hit[4];
			double hit_nrm[3];

			Patch* p = HitTerrain(terrain, ray_p, ray_v, hit, hit_nrm);

			if (p)
			{
				// normalize
				hit_nrm[0] /= HEIGHT_SCALE;
				hit_nrm[1] /= HEIGHT_SCALE;
				double nrm_len = sqrt(hit_nrm[0]*hit_nrm[0]+hit_nrm[1]*hit_nrm[1]+hit_nrm[2]*hit_nrm[2]);
				hit_nrm[0] /= nrm_len;
				hit_nrm[1] /= nrm_len;
				hit_nrm[2] /= nrm_len;
			}

			if (p)
			{
				if (io.KeyAlt)
				{
					if (io.MouseDown[0])
					{
						URDO_Open();
						creating = -1;

						painting_x = (int)roundf(io.MousePos.x);
						painting_y = (int)roundf(io.MousePos.y);

						painting_dx = hit[0];
						painting_dy = hit[1];
					}
					else
					{
						// paint similar preview as for diag flipping but 
						// hilight entire PATCH (instead of quad) and use RED color

						// add here quad preview
						double qx = floor(hit[0] / VISUAL_CELLS) * VISUAL_CELLS;
						double qy = floor(hit[1] / VISUAL_CELLS) * VISUAL_CELLS;
						br_quad[0] = (float)qx;
						br_quad[1] = (float)qy;
						br_quad[2] = -1.0f; // indicates full patch
					}
				}
				else
				if (edit_mode == 0)
				{
					if (io.KeyCtrl)
					{
						if (io.KeyShift)
						{
							// add here probe preview
							if (io.MouseDown[0])
							{
								// height-probe
								probe_z = (int)round(hit[2]);
								br_probe[0] = (float)probe_z;
								br_probe[1] = 0.5f;
							}
							else
							{
								// preview
								br_probe[0] = (float)round(hit[2]);
								br_probe[1] = 0.5f;
							}
						}
						else
						{
							// add here quad preview
							double qx = floor(hit[0] * HEIGHT_CELLS / VISUAL_CELLS) * VISUAL_CELLS / HEIGHT_CELLS;
							double qy = floor(hit[1] * HEIGHT_CELLS / VISUAL_CELLS) * VISUAL_CELLS / HEIGHT_CELLS;
							br_quad[0] = (float)qx;
							br_quad[1] = (float)qy;
							br_quad[2] = 1.0f; // indicates real height quad

							if (!diag_flipped && io.MouseDown[0])
							{
								struct mod_floor
								{
									mod_floor(int d) : y(d) {}
									int mod(int x)
									{
										int r = x % y;
										if (/*(r != 0) && ((r < 0) != (y < 0))*/ r && (r^y)<0) 
											r += y;
										return r;
									}
									int y;
								} mf(HEIGHT_CELLS);

								// floor xy hit coords to height cells
								//int hx = (int)floor(hit[0] * HEIGHT_CELLS / VISUAL_CELLS) % HEIGHT_CELLS;
								//int hy = (int)floor(hit[1] * HEIGHT_CELLS / VISUAL_CELLS) % HEIGHT_CELLS;

								int hx = mf.mod((int)floor(hit[0] * HEIGHT_CELLS / VISUAL_CELLS));
								int hy = mf.mod((int)floor(hit[1] * HEIGHT_CELLS / VISUAL_CELLS));

								{
									uint16_t diag = GetTerrainDiag(p);
									diag ^= 1 << (hx + hy * HEIGHT_CELLS);

									URDO_Diag(p);
									SetTerrainDiag(p, diag);
								}

								// one per click
								diag_flipped = true;
							}
						}
					}
					else
					{
						br_xyra[0] = (float)hit[0];
						br_xyra[1] = (float)hit[1];
						br_xyra[3] = br_alpha;

						if (io.MouseDown[0])
						{
							//BEGIN
							URDO_Open();
							painting = 1;

							painting_x = (int)roundf(io.MousePos.x);
							painting_y = (int)roundf(io.MousePos.y);

							painting_dx = hit[0];
							painting_dy = hit[1];
							paint_dist = 0.0;

							float alpha = br_alpha;
							br_alpha *= STAMP_A;
							Stamp(hit[0], hit[1]);
							br_alpha = alpha;

							// stamped, don't apply preview to it
						}
					}
				}
				else
				if (edit_mode == 1)
				{
					if (io.KeyCtrl)
					{
						if (io.KeyShift)
						{
							// add here probe preview
							if (io.MouseDown[0])
							{
								// height-probe
								probe_z = (int)round(hit[2]);
								br_probe[0] = (float)probe_z;
								br_probe[1] = 0.5f;
							}
							else
							{
								// preview
								br_probe[0] = (float)round(hit[2]);
								br_probe[1] = 0.5f;
							}
						}
						else
						{
							// add here quad preview of matid probe
							double qx = floor(hit[0]);
							double qy = floor(hit[1]);
							br_quad[0] = (float)qx;
							br_quad[1] = (float)qy;
							br_quad[2] = 2.0f; // indicates quad on visual map

							if (io.MouseDown[0])
							{
								struct mod_floor
								{
									mod_floor(int d) : y(d) {}
									int mod(int x)
									{
										int r = x % y;
										if (/*(r != 0) && ((r < 0) != (y < 0))*/ r && (r^y)<0) 
											r += y;
										return r;
									}
									int y;
								} mf(VISUAL_CELLS);

								// sample matid
								int uv[2] = { mf.mod((int)qx), mf.mod((int)qy) };
								uint16_t* visual = GetTerrainVisualMap(p);
								active_material = visual[uv[0] + uv[1]*VISUAL_CELLS] & 0xFF;
							}
						}
					}
					else
					{
						br_xyra[0] = (float)hit[0];
						br_xyra[1] = (float)hit[1];
						br_xyra[2] = (float)br_radius * 0.5f;
						br_xyra[3] = 2; // alpha>1 -> painting matid

						if (br_limit)
						{
							if (io.KeyShift)
								br_probe[2] = -1.0;
							else
								br_probe[2] = 1.0;
						}
						else
							br_probe[2] = 0;

						if (io.MouseDown[0])
						{
							//BEGIN
							URDO_Open();
							painting = 2;

							MatIDStamp stamp;
							stamp.r = br_radius * 0.5;
							stamp.hit = hit;
							stamp.z = br_probe[0];
							stamp.z_lim = br_limit ? (io.KeyShift ? -1 : 1) : 0;

							URDO_Open();
							QueryTerrain(terrain, hit[0], hit[1], br_radius * 0.501, 0x00, MatIDStamp::SetMatCB, &stamp);
							URDO_Close();

							painting_x = (int)roundf(io.MousePos.x);
							painting_y = (int)roundf(io.MousePos.y);

							painting_dx = hit[0];
							painting_dy = hit[1];
							paint_dist = 0.0;
						}
					}
				}
				else
				if (edit_mode == 2)
				{
					if (!inst_added || !io.MouseDown[0])
					{
						// pretranslate and scale
						MeshPrefs* mp = (MeshPrefs*)GetMeshCookie(active_mesh);

						double ptm[16] = {0};
						ptm[0] = pow(2.0,mp->scale_val[0] + 2*mp->scale_rnd[0]*((double)fast_rand() / 0x7fff - 0.5) );
						ptm[5] = pow(2.0,mp->scale_val[1] + 2*mp->scale_rnd[1]*((double)fast_rand() / 0x7fff - 0.5) );
						ptm[10] = pow(2.0,mp->scale_val[2] + 2*mp->scale_rnd[2]*((double)fast_rand() / 0x7fff - 0.5) );
						ptm[15] = 1;
						ptm[12] = 0; //mp->pre_trans[0] * ptm[0];
						ptm[13] = 0; //mp->pre_trans[1] * ptm[5];
						ptm[14] = 0; //mp->pre_trans[2] * ptm[10];

						// rot loc Z
						double ztm[16];
						double loc_z[3] = {0,0,1};
						double ang_z = mp->rotate_locZ_val + 360*mp->rotate_locZ_rnd*((double)fast_rand() / 0x7fff - 0.5);
						Rotation(loc_z, ang_z * M_PI / 180, ztm);

						// rot xy
						double rot[16]; //rtm[16];
						double rot_xy[3] =
						{
							mp->rotate_XY_val[0]/180.0 + 2*mp->rotate_XY_rnd[0]*((double)fast_rand() / 0x7fff - 0.5),
							mp->rotate_XY_val[1]/180.0 + 2*mp->rotate_XY_rnd[1]*((double)fast_rand() / 0x7fff - 0.5),
							0
						};

						double ang_xy = sqrt(rot_xy[0]*rot_xy[0] + rot_xy[1]*rot_xy[1]);
						if (ang_xy != 0)
						{
							rot_xy[0] /= ang_xy;
							rot_xy[1] /= ang_xy;
						}

						if (ang_xy>1)
							ang_xy = 1; 

						Rotation(rot_xy, ang_xy * M_PI, rot/*rtm*/);

						// last thing, align with terrain normal!
						double up[4]={0,0,1,0};
						double dir[4];
						Product(rot,/*rtm,*/up,dir);

						// alignment rot axis
						double align_axis[3];
						CrossProduct(dir,hit_nrm,align_axis);

						// alignment angle
						double align_len = sqrt( align_axis[0]*align_axis[0] + align_axis[1]*align_axis[1] + align_axis[2]*align_axis[2]);
						double align_ang = asin( align_len );

						if (align_len > 0)
						{
							align_axis[0] /= align_len; 
							align_axis[1] /= align_len;
							align_axis[2] /= align_len;
						}

						double atm[16];
						Rotation(align_axis,align_ang * mp->rotate_align,atm);

						double rtm[16];
						MatProduct(atm,rot,rtm);

						double itm[16] = {0};

						// post-scale and translate
						itm[0] = 1;
						itm[5] = 1;
						itm[10] = HEIGHT_SCALE;
						itm[15] = 1;

						itm[12] = hit[0];
						itm[13] = hit[1];
						itm[14] = hit[2];

						double tm1[16];
						double tm2[16];

						// inst_tm = itm * rtm * ztm * ptm
						MatProduct(itm,rtm,tm1);
						MatProduct(ztm,ptm,tm2);
						MatProduct(tm1,tm2,inst_tm);

						if (!inst_added && io.MouseDown[0])
						{
							int flags = INST_USE_TREE | INST_VISIBLE;
							Inst* inst = CreateInst(active_mesh, flags, inst_tm, 0);

							inst_added = true;
							RebuildWorld(world);
						}
						else
						{
							// we'll need to paint active_mesh with inst_tm
							inst_preview = active_mesh;
						}
					}
				}
				else
				if (edit_mode == 3)
				{
					
				}
				else
				if (edit_mode == 4)
				{
					
				}
			}
			else
			{
				if (io.KeyAlt)
				{
					double t = (probe_z - ray_p[2]) / ray_v[2];
					double vx = ray_p[0] + t * ray_v[0];
					double vy = ray_p[1] + t * ray_v[1];

					// probably create 
					if (io.MouseDown[0])
					{
						URDO_Open();
						creating = +1;

						painting_x = (int)roundf(io.MousePos.x);
						painting_y = (int)roundf(io.MousePos.y);

						painting_dx = vx;
						painting_dy = vy;
					}
					else
					{
						create_preview = true;
						create_preview_px = (int)floor(vx / VISUAL_CELLS);
						create_preview_py = (int)floor(vy / VISUAL_CELLS);

						// paint imaginary patch?
						// that requires extra draw command!
					}
				}
			}
		}
	}

	if (panning || spinning)
	{
		br_xyra[3] = 0;
	}

	if (edit_mode==0 && io.KeysDown[A3D_LSHIFT])
	{
		br_xyra[2] = -br_xyra[2];
	}

	// 4 clip planes in clip-space

	double clip_left[4] =   { 1, 0, 0,+1 };
	double clip_right[4] =  {-1, 0, 0,+1 };
	double clip_bottom[4] = { 0, 1, 0,+1 }; 
	double clip_top[4] =    { 0,-1, 0,+1 }; // adjust by max brush descent

	double brush_extent = cos(pitch) * br_xyra[3] * br_xyra[2] / ry;

	if (br_xyra[2] > 0)
	{
		// adjust by max brush ASCENT
		if (br_xyra[3] > 0)
			clip_bottom[3] += brush_extent;

		// adjust by max brush DESCENT
		if (br_xyra[3] < 0)
			clip_top[3] -= brush_extent;
	}

	// transform them to world-space (mul by tm^-1)

	double clip_world[4][4];
	TransposeProduct(tm, clip_left, clip_world[0]);
	TransposeProduct(tm, clip_right, clip_world[1]);
	TransposeProduct(tm, clip_bottom, clip_world[2]);
	TransposeProduct(tm, clip_top, clip_world[3]);

	int planes = 4;
	int view_flags = 0xAA; // should contain only bits that face viewing direction

	double noon_yaw[2] =
	{
		// zero is behind viewer
		-sin(-lit_yaw*M_PI / 180),
		-cos(-lit_yaw*M_PI / 180),
	};

	double dusk_yaw[3] =
	{
		-noon_yaw[1],
		noon_yaw[0],
		0
	};

	double noon_pos[3] =
	{
		noon_yaw[0]*cos(lit_pitch*M_PI / 180),
		noon_yaw[1]*cos(lit_pitch*M_PI / 180),
		sin(lit_pitch*M_PI / 180)
	};

	double lit_axis[3];

	CrossProduct(dusk_yaw, noon_pos, lit_axis);

	double time_tm[16];
	Rotation(lit_axis, (lit_time-12)*M_PI / 12, time_tm);

	double lit_pos[4];
	Product(time_tm, noon_pos, lit_pos);

	float lt[4] =
	{
		(float)lit_pos[0],
		(float)lit_pos[1],
		(float)lit_pos[2],
		ambience
	};

	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_GEQUAL);
	rc->BeginPatches(tm, lt, br_xyra, br_quad, br_probe);
	QueryTerrain(terrain, planes, clip_world, view_flags, RenderContext::RenderPatch, rc);
	rc->EndPatches();


	rc->BeginMeshes(tm, lt);
	QueryWorld(world, planes, clip_world, RenderContext::RenderMesh, rc);
	if (inst_preview)
		RenderContext::RenderMesh(inst_preview, inst_tm, rc);
	rc->EndMeshes();


/*
	rc->BeginBSP(tm);
	QueryWorldBSP(world, planes, clip_world, RenderContext::RenderBSP, rc);
	rc->EndBSP();
*/

	// overlay patch creation
	// slihouette of newly created patch 

	if (create_preview)
	{
		uint16_t ghost[4 * HEIGHT_CELLS];
		bool exist = CalcTerrainGhost(terrain, create_preview_px, create_preview_py, probe_z, ghost);
		if (!exist)
			rc->PaintGhost(tm, create_preview_px, create_preview_py, probe_z, ghost);
	}

	glDisable(GL_DEPTH_TEST);

	//glUseProgram(0); // You may want this if using this code in an OpenGL 3+ context where shaders may be bound, but prefer using the GL3+ code.
	
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

	if (term)
	{
		// bind active font tex
		// bind pal texture
		// use vt program

		// setup uniforms:
		// transform (ortho)
		// font and pal units
		// cell width and height in 2/vp_width, 2/vp_height units

		// set num of glyphs = 0
		
		// for every cell in vt->screen containing char that is defined in font:
		// store in some vbo 4 attribs (16bytes): uint(chr with flags); ushort[2](x,y); ubyte[4](fg); ubyte[4](bg); 
		// increment num of glyphs

		// if vbo buffer is full
		// glBufferSubData and glDrawArrays(GL_POINTS) reset num of glyphs in buffer

	}

	if (term)
	{
		if (a3dDumpVT(term))
		{
			glScissor(0,0,16,16);
			glEnable(GL_SCISSOR_TEST);
			glClearColor(1,1,1,1);
			glClear(GL_COLOR_BUFFER_BIT);
			glDisable(GL_SCISSOR_TEST);
		}
	}

	a3dSwapBuffers();

	if (ipal)
	{
		ClearScreen(screen, 0,0, 256, 256*9/16, 0/*rand*/);
		PrintScreen(screen,ipal);
	}
}

void my_mouse(int x, int y, MouseInfo mi)
{
	#ifdef MOUSE_QUEUE

	// allow overwriting mouse moves
	if (mouse_queue_len)
	{
		MouseQueue* mq = mouse_queue + mouse_queue_len - 1;
		if ((mi & 0xF) == 0 && (mq->mi & 0xF) == 0)
		{
			mq->x = x;
			mq->y = x;
			mq->mi = mi;
			return;
		}
	}

	if (mouse_queue_len==mouse_queue_size)
	{
		mouse_queue_len--;
		for (int i=0; i<mouse_queue_len; i++)
			mouse_queue[i] = mouse_queue[i+1];
	}
	mouse_queue[mouse_queue_len].x = x;
	mouse_queue[mouse_queue_len].y = y;
	mouse_queue[mouse_queue_len].mi = mi;
	mouse_queue_len++;

	#else

	if ((mi & 0xF) == MouseInfo::LEAVE)
	{
		mouse_in = 0;
		return;
	}

	ImGuiIO& io = ImGui::GetIO();

	io.MousePos = ImVec2((float)x, (float)y);

	if ((mi & 0xF) == MouseInfo::ENTER)
		mouse_in = 1;

	switch (mi & 0xF)
	{
		case MouseInfo::WHEEL_DN:
			zoom_wheel--;
			io.MouseWheel -= 1.0;
			break;
		case MouseInfo::WHEEL_UP:
			zoom_wheel++;
			io.MouseWheel += 1.0;
			break;

		default:
			if (mouse_queue_len==mouse_queue_size)
			{
				mouse_queue_len--;
				for (int i=0; i<mouse_queue_len; i++)
					mouse_queue[i] = mouse_queue[i+1];
			}
			mouse_queue[mouse_queue_len++] = mi & 0xF;
			break;

		case MouseInfo::LEFT_DN:
			io.MouseDown[0] = true;
			break;
		case MouseInfo::LEFT_UP:
			io.MouseDown[0] = false;
			break;
		case MouseInfo::RIGHT_DN:
			io.MouseDown[1] = true;
			break;
		case MouseInfo::RIGHT_UP:
			io.MouseDown[1] = false;
			break;
		case MouseInfo::MIDDLE_DN:
			io.MouseDown[2] = true;
			break;
		case MouseInfo::MIDDLE_UP:
			io.MouseDown[2] = false;
			break;
	}

	#endif
}

void my_resize(int w, int h)
{
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)w, (float)h);
}

void my_init()
{

	term = a3dCreateVT(90,30, "/bin/bash", 0, 0);

	printf("RENDERER: %s\n",glGetString(GL_RENDERER));
	printf("VENDOR:   %s\n",glGetString(GL_VENDOR));
	printf("VERSION:  %s\n",glGetString(GL_VERSION));
	printf("SHADERS:  %s\n",glGetString(GL_SHADING_LANGUAGE_VERSION));

	SetScreen(true);


	screen = CreateScreen(512,128);

	world = CreateWorld();

	// scan meshes dir
	struct MeshScan
	{
		static bool Scan(A3D_DirItem item, const char* name, void* cookie)
		{
			if (!(item&A3D_FILE))
				return true;

			char buf[4096];
			snprintf(buf, 4095, "%s/%s", (char*)cookie, name);
			buf[4095] = 0;

			Mesh* m = LoadMesh(world, buf, name);
			if (m)
			{
				MeshPrefs* mp = (MeshPrefs*)malloc(sizeof(MeshPrefs));
				memset(mp,0,sizeof(MeshPrefs));
				SetMeshCookie(m,mp);
			}
			return true;
		}
	};

	char mesh_dirname[] = "./meshes";
	a3dListDir(mesh_dirname, MeshScan::Scan, mesh_dirname);

	active_mesh = GetFirstMesh(world);
	for (int i=0; i<100000; i++)
	{
		double tm[16]=
		{
			0.1,0,0,0,
			0,0.1,0,0,
			0,0,0.1*HEIGHT_SCALE,0,
			(double)(fast_rand()&0x3FF),
			(double)(fast_rand()&0x3FF),
			0*(double)(fast_rand()&0x1F)*HEIGHT_SCALE,
			1
		};
		CreateInst(active_mesh,INST_USE_TREE|INST_VISIBLE,tm,0);
	}
	RebuildWorld(world);

	// todo:
	// build local array of SORTED meshes (similary to palettes)
	// don't rely on order of mesh list!
	// ...

	glCreateTextures(GL_TEXTURE_3D, 1, &pal_tex);
	glTextureStorage3D(pal_tex, 1, GL_RGBA8, 256, 256, 256); // alpha holds pal-indexes!
	glTextureParameteri(pal_tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTextureParameteri(pal_tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTextureParameteri(pal_tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTextureParameteri(pal_tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTextureParameteri(pal_tex, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	Palettize(0);

	MyMaterial::Init();

	char font_dirname[] = "./fonts";
	fonts_loaded = 0;
	a3dListDir(font_dirname, MyFont::Scan, font_dirname);

	MyPalette::Init();
	char pal_dirname[] = "./palettes";
	palettes_loaded = 0;
	a3dListDir(pal_dirname, MyPalette::Scan, pal_dirname);

	g_Time = a3dGetTime();
	render_context.Create();

	glDebugMessageCallback(glDebugCall, 0/*cookie*/);

	// Setup Dear ImGui context
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();

	// keep it startup dir
	{
		char dir[4096];
		a3dGetCurDir(dir,4096);
		snprintf(ini_path,4096,"%s/imgui.ini",dir);
		ini_path[4095]=0;
		io.IniFilename = ini_path;
	}

	io.BackendPlatformName = "imgui_impl_a3d";

	io.KeyMap[ImGuiKey_Tab] = A3D_TAB;
	io.KeyMap[ImGuiKey_LeftArrow] = A3D_LEFT;
	io.KeyMap[ImGuiKey_RightArrow] = A3D_RIGHT;
	io.KeyMap[ImGuiKey_UpArrow] = A3D_UP;
	io.KeyMap[ImGuiKey_DownArrow] = A3D_DOWN;
	io.KeyMap[ImGuiKey_PageUp] = A3D_PAGEUP;
	io.KeyMap[ImGuiKey_PageDown] = A3D_PAGEDOWN;
	io.KeyMap[ImGuiKey_Home] = A3D_HOME;
	io.KeyMap[ImGuiKey_End] = A3D_END;
	io.KeyMap[ImGuiKey_Insert] = A3D_INSERT;
	io.KeyMap[ImGuiKey_Delete] = A3D_DELETE;
	io.KeyMap[ImGuiKey_Backspace] = A3D_BACKSPACE;
	io.KeyMap[ImGuiKey_Space] = A3D_SPACE;
	io.KeyMap[ImGuiKey_Enter] = A3D_ENTER;
	io.KeyMap[ImGuiKey_Escape] = A3D_ESCAPE;
	io.KeyMap[ImGuiKey_A] = A3D_A;
	io.KeyMap[ImGuiKey_C] = A3D_C;
	io.KeyMap[ImGuiKey_V] = A3D_V;
	io.KeyMap[ImGuiKey_X] = A3D_X;
	io.KeyMap[ImGuiKey_Y] = A3D_Y;
	io.KeyMap[ImGuiKey_Z] = A3D_Z;

	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	//ImGui::StyleColorsClassic();

	ImGui_ImplOpenGL3_Init();

	ImWchar range[]={0x0020, 0x03FF, 0};
	pFont = io.Fonts->AddFontFromFileTTF("./fonts/Roboto-Medium.ttf", 16, NULL, range);	
	io.Fonts->Build();

	terrain = CreateTerrain();

	// ALTERNATIVE:
	// terrain = CreateTerrain(int x, int y, int w, int h, uint16_t* data);
	// xywh coords are in patches, so data is w*4+1,h*4+1 !!!!!!!!!!!!!!!!

	const int num1 = 16;
	const int num2 = num1*num1;

	uint32_t* rnd = (uint32_t*)malloc(sizeof(uint32_t)*num2);
	int n = num2;
	for (int i = 0; i < num2; i++)
		rnd[i] = i;

	for (int i = 0; i < num2; i++)
	{
		int r = (fast_rand() + fast_rand()*(FAST_RAND_MAX+1)) % n;

		uint32_t uv = rnd[r];
		rnd[r] = rnd[--n];
		uint32_t u = uv % num1;
		uint32_t v = uv / num1;
		AddTerrainPatch(terrain, u, v, 0/*fast_rand()&0x7F*/);
	}

	free(rnd);

	pos_x = num1 * VISUAL_CELLS / 2;
	pos_y = num1 * VISUAL_CELLS / 2;
	pos_z = 0x0;

	const char* utf8 = "gugu\xC5\xBB\xC3\xB3\xC5\x82\xC4\x87";

	a3dSetTitle(utf8/*"ASCIIID"*/);
	a3dSetIcon("./icons/app.png");
	a3dSetVisible(true);
}

void my_keyb_char(wchar_t chr)
{
	ImGuiIO& io = ImGui::GetIO();
	io.AddInputCharacter((unsigned short)chr);

	if (!term)
		return;

	if (chr==0x7F) // DEL
	{
		a3dWriteVT(term,"\x1B[3~",4);
		return;
	}

	if (chr<0x80)
	{
		char c[1] =
		{
			(char)chr
		};
		a3dWriteVT(term, c,1);
	}
	else
	if (chr<0x800)
	{
		char cc[2] = 
		{ 
			(char)( ((chr>>6)&0x1F) | 0xC0 ), 
			(char)( (chr&0x3f) | 0x80 ) 
		};
		a3dWriteVT(term, cc,2);
	}
	else
	if (chr<0x10000)
	{
		char ccc[3] = 
		{ 
			(char)( ((chr>>12)&0x0F)|0xE0 ), 
			(char)( ((chr>>6)&0x3f) | 0x80 ), 
			(char)( (chr&0x3f) | 0x80 ) 
		};
		a3dWriteVT(term, ccc,3);
	}
	else
	if (chr<0x101000)
	{
		char cccc[4] = 
		{ 
			(char)( ((chr>>18)&0x07)|0xF0 ), 
			(char)( ((chr>>12)&0x3f) | 0x80 ), 
			(char)( ((chr>>6)&0x3f) | 0x80 ), 
			(char)( (chr&0x3f) | 0x80 )
		};
		a3dWriteVT(term, cccc,4);
	}
}

void my_keyb_key(KeyInfo ki, bool down)
{
	ImGuiIO& io = ImGui::GetIO();
	if (ki < IM_ARRAYSIZE(io.KeysDown))
		io.KeysDown[ki] = down;
	
	io.KeysDown[A3D_ENTER] = a3dGetKeyb(A3D_ENTER) || a3dGetKeyb(A3D_NUMPAD_ENTER);
	io.KeyAlt = a3dGetKeyb(A3D_LALT);// || a3dGetKeyb(A3D_RALT);
	io.KeyCtrl = a3dGetKeyb(A3D_LCTRL) || a3dGetKeyb(A3D_RCTRL);
	io.KeyShift = a3dGetKeyb(A3D_LSHIFT) || a3dGetKeyb(A3D_RSHIFT);

	bool DECCKM = a3dGetVTCursorsMode(term);

	if (term && down)
	{
		const char* esc = 0;
		switch (ki)
		{
			// small subset, note there are Ctrl / Shift ... variants too!
			case A3D_F1: esc = "\x1BOP"; break;
			case A3D_F2: esc = "\x1BOQ"; break;
			case A3D_F3: esc = "\x1BOR"; break;
			case A3D_F4: esc = "\x1BOS"; break;

			case A3D_F5: esc = "\x1B[15~"; break;

			case A3D_F6:  esc = "\x1B[17~"; break;
			case A3D_F7:  esc = "\x1B[18~"; break;
			case A3D_F8:  esc = "\x1B[19~"; break;
			case A3D_F9:  esc = "\x1B[20~"; break;
			case A3D_F10: esc = "\x1B[21~"; break;

			case A3D_F11: esc = "\x1B[23~"; break;
			case A3D_F12: esc = "\x1B[24~"; break;

			case A3D_DOWN:  esc = DECCKM ? "\x1BOB" : "\x1B[B"; break; // "\x1B[1;2A"  w/shift ?
			case A3D_UP:    esc = DECCKM ? "\x1BOA" : "\x1B[A"; break; // "\x1B[1;2B" 
			case A3D_RIGHT: esc = DECCKM ? "\x1BOC" : "\x1B[C"; break; // "\x1B[1;2C"
			case A3D_LEFT:  esc = DECCKM ? "\x1BOD" : "\x1B[D"; break; // "\x1B[1;2D"

			case A3D_HOME: esc = DECCKM ? "\x1BOH" : "\x1B[H"; break;
			case A3D_END:  esc = DECCKM ? "\x1BOF" : "\x1B[F"; break;	
		}

		// BEST DOC ABOUT XTERM:
		// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html

		// ------------------------------------------------------------
		// EVEN BETTER ARE REAL WORLD EXPERIMENTS:

		// BASH MODE (DECCKM off)           MC MODE (DECCKM on)
		/*
			f1-f12
			I: \x1BOP  						\x1BOP                      
			I: \x1BOQ						\x1BOQ
			I: \x1BOR						\x1BOR
			I: \x1BOS						\x1BOS
			I: \x1B[15~						\x1B[15~
			I: \x1B[17~						\x1B[17~
			I: \x1B[18~						\x1B[18~
			I: \x1B[19~						\x1B[19~
			I: \x1B[20~						\x1B[20~
			I: \x1B[21~						\x1B[21~
			I: \x1B[23~						\x1B[23~
			I: \x1B[24~						\x1B[24~

			ins hom pgup del end pgdn
			I: \x1B[2~						\x1B[2~
			I: \x1B[H 						\x1BOH
			I: \x1B[5~						\x1B[5~
			I: \x1B[3~						\x1B[3~
			I: \x1B[F 						\x1BOF
			I: \x1B[6~						\x1B[6~

			up dn rt lt
			I: \x1B[A						\x1BOA
			I: \x1B[B						\x1BOB
			I: \x1B[C						\x1BOC
			I: \x1B[D						\x1BOD

			numpad lock:ON (SAME IN BOTH MODES)
			I: /							/
			I: *							*
			I: -							-
			I: 7							7
			I: 8							8
			I: 9							9
			I: +							+
			I: 4							4
			I: 5							5
			I: 6							6
			I: 1							1
			I: 2							2
			I: 3							3
			I: \r							\r
			I: 0							0
			I: ,							,

			numpad lock:OFF (SAME IN BOTH MODES)
			I: /							/
			I: *							*
			I: -							-
			I: \x1B[H						\x1BOH
			I: \x1B[A						\x1BOA
			I: \x1B[5~						\x1B[5~
			I: +							+
			I: \x1B[D						\x1BOD
			I: \x1B[E						\x1BOE
			I: \x1B[C						\x1BOC
			I: \x1B[F						\x1BOF
			I: \x1B[B						\x1BOB
			I: \x1B[6~						\x1B[6~
			I: \r							\r
			I: \x1B[2~						\x1B[2~
			I: \x1B[3~						\x1B[3~


			I: \x1BOR       				\x1BOR				F3
			I: \x1B[1;2R    				\x1B[1;2R			F3+shift
			I: \x1B[1;3R    				\x1B[1;3R			F3+alt
			I: \x1B[1;4R    				\x1B[1;4R			F3+alt+shift
			I: \x1B[1;5R    				\x1B[1;5R			F3+ctrl
			I: \x1B[1;6R    				\x1B[1;6R			F3+ctrl+shift
			I: \x1B[1;7R    				\x1B[1;7R			F3+ctrl+alt
			I: \x1B[1;8R    				\x1B[1;8R			F3+alt+ctrl+shift


			I: \x1B[15~     				\x1B[15~			F5
			I: \x1B[15;2~					\x1B[15;2~			F5+shift
			I: \x1B[15;3~					\x1B[15;3~			F5+alt
			I: \x1B[15;4~					\x1B[15;4~			F3+alt+shift
			I: \x1B[15;5~					\x1B[15;5~			F3+ctrl
			I: \x1B[15;6~					\x1B[15;6~			F3+ctrl+shift
			I: \x1B[15;7~					\x1B[15;7~			F3+ctrl+alt
			I: \x1B[15;8~					\x1B[15;8~			F3+alt+ctrl+shift		

			I: h							I: h				H
			I: H							I: H				H+shift
			I: \x1Bh						I: \x1Bh			H+alt
			I: \x1BH						I: \x1BH			H+alt+shift
			I: \x08							I: \x08				H+ctrl     (so ctrl+letter is just index of it in alphabet?)
			I: \x08							I: \x08				H+ctrl+shift      (SAME as ctrl !!!)
			I: \x1B\x08						I: \x1B\x08			H+ctrl+alt
			I: \x1B\x08						I: \x1B\x08			H+ctrl+alt+shift  (SAME as ctrl+alt !!!)

			// normal						MC MODE (DECCKM on)
			I: \x1B[H 						\x1BOH 				HOME
			I: \x1B[1;2H    				\x1B[1;2H 			HOME+shift
			I: \x1B[1;3H    				\x1B[1;3H 			HOME+alt
			I: \x1B[1;4H    				\x1B[1;4H 			HOME+alt+shift
			I: \x1B[1;5H    				\x1B[1;5H 			HOME+ctrl
			I: \x1B[1;6H    				\x1B[1;6H 			HOME+ctrl+shift
			I: \x1B[1;7H    				\x1B[1;7H 			HOME+ctrl+alt
			I: \x1B[1;8H    				\x1B[1;8H 			HOME+ctrl+alt+shift
		*/

		if (esc)
			a3dWriteVT(term,esc,strlen(esc));
	}
}

void my_keyb_focus(bool set)
{
}

void my_close()
{
	Mesh* m = GetFirstMesh(world);
	while (m)
	{
		MeshPrefs* mp = (MeshPrefs*)GetMeshCookie(m);
		free(mp);
		m = GetNextMesh(m);
	}

	DeleteWorld(world);
	
	URDO_Purge();
	DeleteTerrain(terrain);
	MyFont::Free();
	MyMaterial::Free();

	if (gather)
	{
		if (gather->tmp_x)
			free(gather->tmp_x);
		if (gather->tmp_y)
			free(gather->tmp_y);
		free(gather);
	}

	if (ipal)
	{
		free(ipal);
		ipal = 0;
	}

	DeleteScreen(screen);

	if (term)
		a3dDestroyVT(term);

	a3dClose();

	ImGui_ImplOpenGL3_Shutdown();
	ImGui::DestroyContext();

	render_context.Delete();

	SetScreen(false);
}

/*
void my_ptydata(A3D_PTY* pty)
{
	char buf[4096];
	int len = a3dReadPTY(pty, buf, 4095);
	if (len<=0)
	{
		// cloing here is unsafe yet!
		return;
	}

	buf[len]=0;

	// intercept : \x1B[?1h 

	for (int i=0; i<len; i++)
	{
		if (buf[i] == '\x1B') // esc
		{
			if (buf[i+1] == '[') // CSI
			{
				if (buf[i+2] == '?') // DEC Private Mode Set (DECSET)
				{
					if (buf[i+3] == '1') // DECCKM
					{
						if (buf[i+4] == 'h')
							DECCKM = true;
						else
						if (buf[i+4] == 'l')
							DECCKM = false;
					}
				}
			}
		}
	}


	if (len)
		write(STDOUT_FILENO, buf, len);
}
*/

int main(int argc, char *argv[]) 
{
	PlatformInterface pi;
	pi.close = my_close;
	pi.render = my_render;
	pi.resize = my_resize;
	pi.init = my_init;
	pi.keyb_char = my_keyb_char;
	pi.keyb_key = my_keyb_key;
	pi.keyb_focus = my_keyb_focus;
	pi.mouse = my_mouse;

	// pi.ptydata = my_ptydata;

	GraphicsDesc gd;
	gd.color_bits = 32;
	gd.alpha_bits = 8;
	gd.depth_bits = 24;
	gd.stencil_bits = 8;
	gd.flags = (GraphicsDesc::FLAGS) (GraphicsDesc::DEBUG_CONTEXT | GraphicsDesc::DOUBLE_BUFFER);

	int rc[] = {0,0,1920*2,1080+2*1080};
	gd.wnd_mode = A3D_WND_NORMAL;
	gd.wnd_xywh = 0;

	a3dOpen(&pi, &gd);

	return 0;
}
