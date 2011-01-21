/*
 *  gpu.fft~.c
 *
 *  Copyright 2010 Toshiro Yamada. All rights reserved.
 */

#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"

#include <OpenCL/opencl.h>
#include "clFFT.h"

#ifdef FORWARD_EXTERNAL
#define EXTERNAL_NAME ("gpu.fft~")
#else // INVERSE_EXTERNAL
#define EXTERNAL_NAME ("gpu.ifft~")
#endif

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

typedef struct _gpu_fft
{
	t_pxobject obj;
	cl_core core;
	clfft_data fft_data;
	long fft_size;
	long cl_device;
	long new_device;
#ifdef FORWARD_EXTERNAL
#else // INVERSE_EXTERNAL
	double one_over_fft_size;
#endif
	char fft_size_has_changed;
	char has_valid_device;
} t_gpu_fft;

static t_class* s_gpu_fft_class = NULL;

//-----------------------------------------------------------------------------
// Private Methods

void notify_callback(const char *errinfo, const void *private_info, size_t cb, 
					 void *user_data)
{
    error("%s: %s\n", EXTERNAL_NAME, errinfo);
}


cl_int clSetupRoutine(cl_core* cl)
{
	cl_int err;
	
	err = clGetDeviceIDs(NULL, cl->device_type, 1, &cl->device_id, 
						 &cl->num_devices);
	if (err) {
		return -1;
	}
	
	cl->context = clCreateContext(0, 1, &cl->device_id, notify_callback, 
								  NULL, &err);
	if (!cl->context || err) {
		return -1;
	}
	
    cl->queue = clCreateCommandQueue(cl->context, cl->device_id, 0, &err);
    if (!cl->queue || err) {
		clReleaseContext(cl->context);
        return -1;
    }  
	
	err = clGetDeviceInfo(cl->device_id, CL_DEVICE_GLOBAL_MEM_SIZE, 
						  sizeof(cl_ulong), &cl->gMemSize, NULL);
	if (err) {
		clReleaseContext(cl->context);
		clReleaseCommandQueue(cl->queue);
		return -2;
	}
	
	// No error
	return 0;
}

cl_int clSetupFFTData(cl_context context, clfft_data* data, long length) 
{
	cl_int err;
	clFFT_Dim3 n = {length, 1, 1};
	
	data->data_i = (clFFT_Complex*) malloc(length * sizeof(clFFT_Complex));
	if (!data->data_i) {
		return -1;
	}
	
	// Create FFT Plan
	data->plan = clFFT_CreatePlan(context, 
								  n,
								  clFFT_1D, 
								  clFFT_InterleavedComplexFormat,
								  &err);
	if (!data->plan || err) {
		// Error creating plan
		free(data->data_i);
		return -1;
	}
	
	data->data_in = clCreateBuffer(context, 
								   CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
								   length * sizeof(clFFT_Complex),
								   data->data_i,
								   &err);
	if (!data->data_in) {
		// error clCreateBuffer
		clFFT_DestroyPlan(data->plan);
		free(data->data_i);
		return -1;
	}
	
	data->data_out = data->data_in;
	data->dim = n;
	
	return 0;
}

cl_int clResizeFFTData(cl_context context, clfft_data* data, long length)
{
	cl_int err;
	clFFT_Dim3 n = {length, 1, 1};
	
	clFFT_DestroyPlan(data->plan);
	err = clReleaseMemObject(data->data_in);
	
	data->data_i = (clFFT_Complex*) realloc(data->data_i, 
											length * sizeof(clFFT_Complex));
	if (!data->data_i) {
		return -1;
	}
	
	// Create FFT Plan
	data->plan = clFFT_CreatePlan(context, 
								  n,
								  clFFT_1D, 
								  clFFT_InterleavedComplexFormat,
								  &err);
	if (!data->plan || err) {
		// Error creating plan
		free(data->data_i);
		return -1;
	}
	
	data->data_in = clCreateBuffer(context, 
								   CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
								   length * sizeof(clFFT_Complex),
								   data->data_i,
								   &err);
	if (!data->data_in) {
		// error clCreateBuffer
		clFFT_DestroyPlan(data->plan);
		free(data->data_i);
		return -1;
	}
	
	data->data_out = data->data_in;
	data->dim = n;
	
	return 0;
}

void clClearFFTData(clfft_data* data)
{
	
	if (data->data_i) {
		free(data->data_i);
	}
	if (data->plan) {
		clFFT_DestroyPlan(data->plan);
	}
	if (data->data_in) {
		clReleaseMemObject(data->data_in);
	}
}


unsigned int nextPowerOf2(unsigned int x)
{
	x--;
	x |= x >> 1;  // handle  2 bit numbers
	x |= x >> 2;  // handle  4 bit numbers
	x |= x >> 4;  // handle  8 bit numbers
	x |= x >> 8;  // handle 16 bit numbers
	x |= x >> 16; // handle 32 bit numbers
	x++;
	
	return x;
}

//-----------------------------------------------------------------------------
void* gpu_fft_new(t_symbol* s, long argc, t_atom* argv)
{
	t_gpu_fft* x;
	t_dictionary* d = object_dictionaryarg(argc, argv);
	
	if (d == NULL) {
		return NULL;
	}
	
	x = (t_gpu_fft*) object_alloc(s_gpu_fft_class);
	
	if (x != NULL) {
		/* Create inlet */
		dsp_setup((t_pxobject*)x, 1);
		/* Create outlet */
		outlet_new((t_object*)x, "signal");
		/* This needs to be called AFTER making inlets/outlets! */
		x->obj.z_misc = Z_NO_INPLACE;
		
		x->new_device = -1;
		x->fft_size_has_changed = FALSE;
		x->has_valid_device = FALSE;
		
		attr_dictionary_process(x, d);
		
//		err = clSetupFFTData(x->core.context, &x->fft_data, x->fft_size);
	}
	
	return x;
}

void gpu_fft_free(t_gpu_fft* x) 
{
	/* Do any deallocation needed here. */
	dsp_free((t_pxobject*)x);	/* Must call this first! */
	
	if (x->has_valid_device) {
		clClearFFTData(&x->fft_data);
		clReleaseContext(x->core.context);
		clReleaseCommandQueue(x->core.queue);
	}
}

t_int* gpu_fft_perform(t_int* w)
{
	t_gpu_fft* x = (t_gpu_fft*)(w[1]);
	t_sample* time_sig = (t_sample*)(w[2]);
	long time_sig_size = (long)(w[3]);
	t_sample* fft_sig = (t_sample*)(w[4]);
	long fft_sig_size = (long)(w[5]) / 2;
	long fft_byte_size = fft_sig_size * sizeof(clFFT_Complex);
	long i = 0;
	cl_int err;
//	cl_event events[2];
	
	// Copy msp input to our local memory
#ifdef FORWARD_EXTERNAL
	for (i = 0; i < fft_sig_size; ++i) {
		x->fft_data.data_i[i].real = 0.0;
		x->fft_data.data_i[i].imag = 0.0;
	}
	for (i = 0; i < time_sig_size; ++i) {
		x->fft_data.data_i[i].real = (float)time_sig[i];
		x->fft_data.data_i[i].imag = 0.0;
	}
#else // INVERSE_EXTERNAL
	memcpy(x->fft_data.data_i, fft_sig, fft_byte_size);
#endif
	
	// Copy memory from host to device
	err = clEnqueueWriteBuffer(x->core.queue, x->fft_data.data_in,
							   CL_TRUE, 0, fft_byte_size, x->fft_data.data_i, 
							   0, NULL, NULL);
//							   0, NULL, &events[0]);
	if (err < 0) {
		goto zero;
	}
	
	err = clFFT_ExecuteInterleaved(x->core.queue, x->fft_data.plan, 
#ifdef FORWARD_EXTERNAL
								   1, clFFT_Forward, 
#else // INVERSE_EXTERNAL
								   1, clFFT_Inverse, 
#endif
								   x->fft_data.data_in, x->fft_data.data_out, 
								   0, NULL, NULL);
//									1, events, &events[1]);
	if (err < 0) {
		goto zero;
	}
	
	err = clEnqueueReadBuffer(x->core.queue, x->fft_data.data_out, 
							  CL_TRUE, 0, fft_byte_size, x->fft_data.data_i, 
							  0, NULL, NULL);
//							  2, events, NULL);
	if (err < 0) {
		goto zero;
	}
	
	// Copy result to outlet
#ifdef FORWARD_EXTERNAL
	memcpy(fft_sig, x->fft_data.data_i, fft_byte_size);
#else // INVERSE_EXTERNAL
	for (i = 0; i < time_sig_size; ++i) {
		time_sig[i] = x->fft_data.data_i[i].real * x->one_over_fft_size;
	}
#endif
	
	return w + 6;
	
zero:
#ifdef FORWARD_EXTERNAL
	memset(fft_sig, 0, fft_byte_size);
#else // INVERSE_EXTERNAL
	memset(time_sig, 0, time_sig_size * sizeof(t_signal));
#endif
	return w + 6;
}

void gpu_fft_dsp(t_gpu_fft* x, t_signal** sp, short* count) 
{	
	cl_core core;
	cl_int err;
	t_signal* fft_sig;
#ifdef FORWARD_EXTERNAL
	t_sample* fft_vec;
	fft_sig = sp[1];
#else // INVERSE_EXTERNAL
	char parsestr[16];
	fft_sig = sp[0];
#endif
	
	if (x->new_device >= 0) {
		core.device_type = x->core.device_type;
		err = clSetupRoutine(&core);
		if (err < 0) {
			x->has_valid_device = FALSE;
			object_error((t_object*)x, 
						 "Cannot setup device for using OpenCL!");
		}
		else {
			DEBUG_POST("Device is now set to %d", x->new_device);
			if (x->has_valid_device) {
				clClearFFTData(&x->fft_data);
				clReleaseContext(x->core.context);
				clReleaseCommandQueue(x->core.queue);
			}
			x->has_valid_device = TRUE;
			x->cl_device = x->new_device;
			x->core = core;
			err = clSetupFFTData(x->core.context, &x->fft_data, x->fft_size);
			if (err < 0) {
				object_error((t_object*)x, 
							 "Error allocating memory for context!");
				x->has_valid_device = FALSE;
				clReleaseContext(x->core.context);
				clReleaseCommandQueue(x->core.queue);
			}
		}
		x->new_device = -1;
	}
	
	if (x->has_valid_device) {
#ifdef FORWARD_EXTERNAL
		if (fft_sig->s_n != (x->fft_size * 2)) {
			fft_vec = t_resizebytes((char*)fft_sig->s_vec, 
									fft_sig->s_n * sizeof(t_sample), 
									x->fft_size * sizeof(clFFT_Complex));
			if (!fft_vec) {
				object_error((t_object*)x, "Cannot resize vector for FFT!");
				return;
			}
			fft_sig->s_vec = fft_vec;
			fft_sig->s_n = x->fft_size * 2;
		}
#else // INVERSE_EXTERNAL
		if (fft_sig->s_n != (x->fft_size * 2)) {
			// Resize FFT data only if the current size is smaller
			if (fft_sig->s_n > (x->fft_size * 2)) {
				err = clResizeFFTData(x->core.context, &x->fft_data, 
									  fft_sig->s_n / 2);
				if (err < 0) {
					// TODO: keep track if we have a valid memory
					// so we can do perform or not depending on this...
					object_error((t_object*)x, "Error resizing memory!");
				}
			}
			sprintf(parsestr, "%ld", fft_sig->s_n/2);
			object_attr_setlong(x, gensym("size"), fft_sig->s_n/2);
		}
#endif
		
		dsp_add(gpu_fft_perform, 5, 
#ifdef FORWARD_EXTERNAL
				x, sp[0]->s_vec, sp[0]->s_n, sp[1]->s_vec, sp[1]->s_n);
#else // INVERSE_EXTERNAL
				x, sp[1]->s_vec, sp[1]->s_n, sp[0]->s_vec, sp[0]->s_n);
#endif
	}
	else {
		object_error((t_object*)x, 
					 "Cannot run DSP because there's no valid device");
	}
}


void gpu_fft_assist(t_gpu_fft* x, void* b, long m, long a, char* s)
{
	if (m == ASSIST_INLET) {
		switch (a) {
			case 0:
#ifdef FORWARD_EXTERNAL
				sprintf(s, "Audio signal");
#else // INVERSE_EXTERNAL
				sprintf(s, "FFT signal");
#endif
				break;
		}
	} 
	else {
		switch (a) {
			case 0:
#ifdef FORWARD_EXTERNAL
				sprintf(s, "FFT signal");
#else // INVERSE_EXTERNAL
				sprintf(s, "Audio signal");
#endif
				break;
		}		
	}
}

t_max_err attr_set_fft_size(t_gpu_fft* x, void* attr, long argc, t_atom* argv)
{
	long in, next;
	
	if (argc > 0 && argv) {
		if (atom_gettype(argv) == A_LONG) {
			// Check power of 2
			in = atom_getlong(argv);
			next = nextPowerOf2(in);
			if (in != next) {
				object_post((t_object*)x, "Setting to the next power of 2: %d",
							next);
			}
			
			x->fft_size = next;
#ifdef INVERSE_EXTERNAL
			x->one_over_fft_size = 1. / (double)x->fft_size;
#endif
		}
	}
	else {
		return MAX_ERR_GENERIC;
	}
	
	return MAX_ERR_NONE;
}

t_max_err attr_set_device(t_gpu_fft* x, void* attr, long argc, t_atom* argv)
{
	long device;
	cl_device_type device_type;
	
	if (argc > 0 && argv) {
		if (atom_gettype(argv) == A_LONG) {
			device = atom_getlong(argv);
			
			if (x->has_valid_device && device == x->cl_device) {
				return MAX_ERR_NONE;
			}
			
			switch (device) {
				case DEVICE_CPU:
					device_type = CL_DEVICE_TYPE_CPU;
					break;
				case DEVICE_GPU:
					device_type = CL_DEVICE_TYPE_GPU;
					break;
				case DEVICE_ACCELERATOR:
					// TODO: Cause a crash when using accelerator mode...
					// device_type = CL_DEVICE_TYPE_ACCELERATOR;
					object_post((t_object*)x, 
								"Accerlator device is disabled. Using default.");
					device_type = CL_DEVICE_TYPE_DEFAULT;
					break;
				case DEVICE_ALL:
					device_type = CL_DEVICE_TYPE_ALL;
					break;
				default:
					object_error((t_object*)x, 
								 "Unkown type! Using the default device.");
				case DEVICE_DEFAULT:
					device_type = CL_DEVICE_TYPE_DEFAULT;
					break;
			}
			
			x->core.device_type = device_type;
			x->new_device = device;
		}
	}
	else {
		return MAX_ERR_GENERIC;
	}
	
	return MAX_ERR_NONE;
}

int main(void)
{
	t_class* c;
	
	c = class_new(EXTERNAL_NAME, (method)gpu_fft_new, (method)gpu_fft_free, 
				  sizeof(t_gpu_fft), (method)0L, A_GIMME, 0);
	
	c->c_flags |= CLASS_FLAG_NEWDICTIONARY;
	
	/* Add class methods */
    class_addmethod(c, (method)gpu_fft_dsp, "dsp", A_CANT, 0);
    class_addmethod(c, (method)gpu_fft_assist, "assist", A_CANT, 0);
	
	/* Add attributes */
#ifdef FORWARD_EXTERNAL
	CLASS_ATTR_LONG(c, "size", 0, t_gpu_fft, fft_size);
	CLASS_ATTR_DEFAULT_SAVE(c, "size", 0, "8192");
#else // INVERSE_EXTERNAL
	CLASS_ATTR_LONG(c, "size", ATTR_SET_OPAQUE_USER, t_gpu_fft, fft_size);
	CLASS_ATTR_DEFAULT(c, "size", 0, "8192");
#endif
	CLASS_ATTR_ACCESSORS(c, "size", NULL, attr_set_fft_size);
	CLASS_ATTR_LABEL(c, "size", 0, "Size of FFT (power of 2)");
	
	CLASS_ATTR_LONG(c, "device", 0, t_gpu_fft, cl_device);
	CLASS_ATTR_ENUMINDEX(c, "device", 0, ATTR_DEVICE_ENUM);
	CLASS_ATTR_ACCESSORS(c, "device", NULL, attr_set_device);
	CLASS_ATTR_LABEL(c, "device", 0, "Device to perform FFT");
	CLASS_ATTR_DEFAULT_SAVE(c, "device", 0, "2");
	
	CLASS_ATTR_CHAR(c, "has_valid_device", ATTR_SET_OPAQUE_USER, t_gpu_fft, 
					has_valid_device);
	CLASS_ATTR_STYLE_LABEL(c, "has_valid_device", 0, "onoff", 
						   "Is device set and ready to rock?");
	
	class_dspinit(c);
	class_register(CLASS_BOX, c);
	s_gpu_fft_class = c;
	
	return 0;
}
