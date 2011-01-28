/*
 *  gpu.fft~.c
 *
 *  Copyright (C) 2010 Toshiro Yamada. All rights reserved.
 */

#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"

#define EXTERNAL_NAME "sa.gpu.ifft~"

#ifdef DEBUG
#define DEBUG_POST(...) object_post((t_object*)x, "DEBUG: " __VA_ARGS__);
#else
#define DEBUG_POST(...)
#endif

#define ATTR_DEVICE_ENUM "Default CPU GPU Accelerator All"
enum {
	DEVICE_DEFAULT = 0,
	DEVICE_CPU,
	DEVICE_GPU,
	DEVICE_ACCELERATOR,
	DEVICE_ALL
}; 

typedef struct {
	cl_device_type device_type;
	unsigned int num_devices;
	cl_device_id device_id;
	cl_context context;
	cl_command_queue queue;
	cl_ulong gMemSize;
} cl_core;

typedef struct {
	clFFT_Dim3 dim;
	clFFT_Complex* data_i;
	clFFT_Plan plan;
	cl_mem data_in;
	cl_mem data_out;
} clfft_data;

typedef struct _gpu_ifft
{
	t_pxobject obj;
	long fft_size;
} t_gpu_ifft;

static t_class* s_gpu_ifft_class = NULL;

void* gpu_ifft_new(t_symbol* s, long argc, t_atom* argv)
{
	t_dictionary* d = object_dictionaryarg(argc, argv);
	
	if (d == NULL) {
		return NULL;
	}
	
	t_gpu_ifft* x = (t_gpu_ifft*) object_alloc(s_gpu_ifft_class);
	
	if (x != NULL) {
		attr_dictionary_process(x, d);
		
		/* Create inlet */
		dsp_setup((t_pxobject*)x, 1);
		/* Create outlet */
		outlet_new((t_object*)x, "signal");
		/* This needs to be called AFTER making inlets/outlets! */
		x->obj.z_misc = Z_NO_INPLACE;
	}
	
	return x;
}

void gpu_ifft_free(t_gpu_ifft* x) 
{
	/* Do any deallocation needed here. */
	dsp_free((t_pxobject*)x);	/* Must call this first! */
}

t_int* gpu_ifft_perform(t_int* w)
{
//	t_gpu_ifft* x = (t_gpu_ifft*)(w[1]);
	t_float* in = (t_float*)(w[2]);
	int in_size = (int)(w[3]);
	t_float* out = (t_float*)(w[4]);
	int out_size = (int)(w[5]);
	int i = 0;
	
	for (i = 0; i < in_size; ++i) {
		out[i % out_size] = *in++;
	}
	
	return w + 6;
}

void gpu_ifft_dsp(t_gpu_ifft* x, t_signal** sp, short* count) 
{
//	t_sample* sig;
	
	object_post((t_object*)x, "in size: %d, out size: %d", sp[0]->s_n, sp[1]->s_n);
	
	object_post((t_object*)x, "&sp[0]->s_vec = %p, &sp[1]->s_vec = %p",
				sp[0]->s_vec, sp[1]->s_vec);
	
	dsp_add(gpu_ifft_perform, 5, 
			x, sp[0]->s_vec, sp[0]->s_n, sp[1]->s_vec, sp[1]->s_n);
}


void gpu_ifft_assist(t_gpu_ifft* x, void* b, long m, long a, char* s)
{
	if (m == ASSIST_INLET) {
		switch (a) {
			case 0:
				sprintf(s, "FFT signal");
				break;
		}
	} 
	else {
		switch (a) {
			case 0:
				sprintf(s, "Audio signal");
				break;
		}		
	}
}

/*
t_max_err attr_set_fft_size(t_gpu_ifft* x, void* attr, long argc, t_atom* argv)
{
	if (argc > 0 && argv) {
		if (atom_gettype(argv) == A_LONG) {
			// Check power of 2
			x->fft_size = atom_getlong(argv);
		}
	}
	else {
		return MAX_ERR_GENERIC;
	}
	
	return MAX_ERR_NONE;
}
 */

int main(void)
{
	t_class* c;
	
	c = class_new(EXTERNAL_NAME, (method)gpu_ifft_new, (method)gpu_ifft_free, 
				  sizeof(t_gpu_ifft), (method)0L, A_GIMME, 0);
	
	c->c_flags |= CLASS_FLAG_NEWDICTIONARY;
	
	/* Add class methods */
    class_addmethod(c, (method)gpu_ifft_dsp, "dsp", A_CANT, 0);
    class_addmethod(c, (method)gpu_ifft_assist, "assist", A_CANT, 0);
	
	/* Add attributes */
//	CLASS_ATTR_LONG(c, "size", 0, t_gpu_ifft, fft_size);
//	CLASS_ATTR_ACCESSORS(c, "size", NULL, attr_set_fft_size);
//	CLASS_ATTR_LABEL(c, "size", 0, "Size of FFT (rounds to power of 2)");
	
	class_dspinit(c);
	class_register(CLASS_BOX, c);
	s_gpu_ifft_class = c;
	
	return 0;
}
