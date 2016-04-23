#include "ccv_nnc.h"
#include <time.h>
#include <sys/time.h>

#ifdef __MACH__
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

typedef void(*ccv_nnc_init_f)(ccv_nnc_cmd_api_t cmd_api[]);

typedef struct {
	int backend;
	ccv_nnc_init_f init;
} ccv_nnc_init_t;

#define CCV_NNC_INIT_DECL(init_func) extern void (init_func)(ccv_nnc_cmd_api_t cmd_api[])
#define CCV_NNC_INIT_MAP_BEGIN() static ccv_nnc_init_t init_map[] = {
#define CCV_NNC_INIT_MAP(name, init_func) { .backend = name, .init = init_func, },
#define CCV_NNC_INIT_MAP_END() };

#define CCV_NNC_INIT_EXEC(name, init_func) do { \
		(init_func)(command_api_decls[name]); \
	} while (0)

// Below is extracted from source code: `./nnc-init.rb cpu` or `./nnc-init.rb cpu gpu`
#ifdef HAVE_CUDA // Include the header for both CPU and GPU
#include "ccv_nnc_init.inc"
#else // Otherwise only include for CPU.
#include "cpu/ccv_nnc_init.inc"
#endif
// Above should be automatic generated.

static ccv_nnc_cmd_api_t cmd_api_decls[CCV_NNC_BACKEND_COUNT][CCV_NNC_COMPUTE_COUNT];

void ccv_nnc_init(void)
{
	int i;
	int count = sizeof(init_map) / sizeof(ccv_nnc_init_t);
	// Init dynamic dispatch table.
	for (i = 0; i < count; i++)
		init_map[i].init(cmd_api_decls[init_map[i].backend]);
}

const ccv_nnc_cmd_param_t ccv_nnc_default_cmd_params = {{{0}}};

ccv_nnc_cmd_t ccv_nnc_cmd(const int compute, ccv_nnc_cmd_exec_f exec, const ccv_nnc_cmd_param_t params, const int flags)
{
	ccv_nnc_cmd_t cmd;
	cmd.info = params;
	// Default to CPU ref implementation if the type is CPU memory, otherwise use GPU ref.
	cmd.backend = CCV_NNC_BACKEND_CPU_REF;
	assert((compute == CCV_NNC_COMPUTE_CUSTOM && exec) || (compute != CCV_NNC_COMPUTE_CUSTOM && !exec));
	cmd.compute = compute;
	cmd.algorithm = -1; // This is default.
	cmd.exec = exec;
	return cmd;
}

const ccv_nnc_hint_t ccv_nnc_default_hint = {{{0}}};

int ccv_nnc_hint_verify(const ccv_nnc_hint_t hint, const ccv_nnc_cmd_param_t cmd, const ccv_nnc_tensor_param_t a, const ccv_nnc_tensor_param_t b)
{
	int i;
	// 0-dim is reserved for channels
	for (i = 1; i < CCV_NNC_MAX_DIM + 1; i++)
	{
		if ((hint.border.begin[i] + hint.border.end[i] + a.dim[i] - cmd.size.dim[i]) % hint.stride.dim[i] != 0)
			return -1;
		int expected = (hint.border.begin[i] + hint.border.end[i] + a.dim[i] - cmd.size.dim[i]) / hint.stride.dim[i] + 1;
		if (expected != b.dim[i])
			return -1;
	}
	return 0;
}

ccv_nnc_hint_t ccv_nnc_hint_guess(const ccv_nnc_cmd_param_t cmd, const ccv_nnc_tensor_param_t* inputs, const int input_size, const ccv_nnc_tensor_param_t* outputs, const int output_size)
{
	ccv_nnc_hint_t guess;
	guess.stride.dim[0] = 0;
	guess.border.begin[0] = 0;
	guess.border.end[0] = 0;
	assert(input_size == 1);
	assert(output_size == 1);
	const ccv_nnc_tensor_param_t a = inputs[0];
	const ccv_nnc_tensor_param_t b = outputs[0];
	int i;
	// 0-dim is reserved for channels
	for (i = 1; i < CCV_NNC_MAX_DIM + 1; i++)
	{
		// This is guessed by having a stride that will approximately match the scale.
		int stride = (a.dim[i] + b.dim[i] / 2) / b.dim[i];
		guess.stride.dim[i] = stride;
		int border = (b.dim[i] - 1) * stride - a.dim[i] + cmd.size.dim[i];
		guess.border.begin[i] = border / 2;
		guess.border.end[i] = border - guess.border.begin[i];
	}
	return guess;
}

// This returns absolute time.
uint64_t ccv_nnc_cmd_absolute_time(void)
{
#ifdef __MACH__
	return mach_absolute_time();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

#define AUTO_TUNE_TRIAL_SIZE (3)

ccv_nnc_cmd_t ccv_nnc_cmd_autotune(const ccv_nnc_cmd_t cmd, const ccv_nnc_hint_t hint, const int flags, ccv_nnc_tensor_t* const* inputs, const int input_size, ccv_nnc_tensor_t** outputs, const int output_size, const ccv_nnc_stream_unit_t* stream_unit)
{
	// This is a custom compute kernel, no need to autotune.
	if (cmd.compute == CCV_NNC_COMPUTE_CUSTOM)
		return cmd;
	int i, j, k;
	// Go through all the backends that supports the same type of memory input / output tensors support.
	int tensor_memory = 0;
	for (i = 0; i < input_size; i++)
		tensor_memory |= inputs[i]->info.type;
	for (i = 0; i < output_size; i++)
		tensor_memory |= outputs[i]->info.type;
	// In this case, we cannot determine the type of the tensor, skip auto-tune.
	if (!tensor_memory)
		return cmd;
	// Otherwise, we are good to go.
	ccv_nnc_cmd_t tuned_cmd = cmd;
	int64_t best_measured = -1;
	// We need to have trial loop through all the data.
	for (k = 0; k < AUTO_TUNE_TRIAL_SIZE; k++)
	{
		for (i = 0; i < CCV_NNC_BACKEND_COUNT; i++)
		{
			// We have the exec kernel, and support all the tensor memory types.
			ccv_nnc_cmd_api_t api_decl = cmd_api_decls[i][cmd.compute];
			if (api_decl.exec &&
				(api_decl.tensor_memory & tensor_memory) == tensor_memory)
			{
				ccv_nnc_cmd_t candid_cmd = cmd;
				candid_cmd.backend = i;
				for (j = 0; j < api_decl.algorithms; j++)
				{
					candid_cmd.algorithm = j;
					uint64_t elapsed = ccv_nnc_cmd_absolute_time();
					// Ready to run.
					int status = ccv_nnc_cmd_exec(candid_cmd, hint, flags, inputs, input_size, outputs, output_size, stream_unit);
					elapsed = ccv_nnc_cmd_absolute_time() - elapsed;
					if (status == CCV_NNC_EXEC_SUCCESS &&
						(best_measured == -1 || elapsed < best_measured))
					{
						best_measured = elapsed;
						tuned_cmd = candid_cmd;
					}
				}
			}
		}
	}
	return tuned_cmd;
}

int ccv_nnc_cmd_exec(const ccv_nnc_cmd_t cmd, const ccv_nnc_hint_t hint, const int flags, ccv_nnc_tensor_t* const* inputs, const int input_size, ccv_nnc_tensor_t** outputs, const int output_size, const ccv_nnc_stream_unit_t* stream_unit)
{
	assert(cmd.backend < CCV_NNC_BACKEND_COUNT);
	assert(cmd.compute < CCV_NNC_COMPUTE_COUNT);
	// If it is a custom command, just apply it directly.
	if (cmd.compute == CCV_NNC_COMPUTE_CUSTOM)
		return cmd.exec(cmd, hint, flags, inputs, input_size, outputs, output_size, stream_unit);
	ccv_nnc_cmd_api_t api_decl = cmd_api_decls[cmd.backend][cmd.compute];
	if (!api_decl.exec)
		return CCV_NNC_EXEC_NO_KERNEL;
	int i;
	for (i = 0; i < input_size; i++)
	{
		assert(api_decl.tensor_formats & inputs[i]->info.format);
	}
	for (i = 0; i < output_size; i++)
	{
		assert(api_decl.tensor_formats & outputs[i]->info.format);
	}
	return api_decl.exec(cmd, hint, flags, inputs, input_size, outputs, output_size, stream_unit);
}
