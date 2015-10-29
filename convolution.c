/*
 * convolution.c
 *
 *  Created on: Sep 20, 2015
 *      Author: Dawson
 */

#define NANOSECONDS_IN_A_SECOND			1000000000
#define NUM_CHECKS_PER_CYCLE			2
#define FFT_SIZE MIN_FFT_BLOCK_SIZE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <sndfile.h>
#include <math.h>
#include <time.h>
#include <limits.h>
#include <dirent.h>
#include <unistd.h>
#include <portaudio.h>
#include <ncurses.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <pthread.h>
#include "dawsonaudio.h"
#include "convolve.h"
#include "vector.h"
#include "impulse.h"
#include <GLUT/glut.h>

GLsizei g_width = 800;
GLsizei g_height = 600;
GLsizei g_last_width = 800;
GLsizei g_last_height = 600;
GLfloat g_mouse_x;
GLfloat g_mouse_y;
GLfloat g_relative_width = 8.0f;
GLfloat g_height_top = 3;
GLfloat g_height_bottom = -3;

GLenum g_fillmode = GL_FILL;

GLboolean g_fullscreen = false;
GLboolean g_drawempty = false;
GLboolean g_ready = false;
GLboolean a_pressed = false;
GLboolean z_pressed = false;

GLfloat g_angle_y = 0.0f;
GLfloat g_angle_x = 0.0f;
GLfloat g_inc = 0.0f;
GLfloat g_inc_y = 0.0f;
GLfloat g_inc_x = 0.0f;
GLfloat g_linewidth = 1.0f;

float top_vals[FFT_SIZE];
float bottom_vals[FFT_SIZE];

unsigned int g_channels = MONO;

typedef struct FFTArgs {
	int first_sample_index;
	int last_sample_index;
	int impulse_block_number;
	int num_callbacks_to_complete;
	int counter;
} FFTArgs;

FFTData *g_fftData_ptr; // Stores the Fourier-transforms of each block of the impulse

int g_block_length; // The length in frames of each audio buffer received by portaudio
int g_impulse_length; // The length in frames of the impulse
int g_num_blocks; // The number of equal-size blocks into which the impulse will be divided
int g_max_factor; // The highest power of 2 used to divide the impulse into blocks
int g_input_storage_buffer_length; // The length of the buffer used to store incoming audio from the mic
int g_output_storage_buffer1_length; // channel 1
int g_output_storage_buffer2_length; // channel 2
int g_end_sample; // The index of the last sample in g_storage_buffer
int g_counter = 0; // Keep track of how many callback cycles have passed

long g_block_duration_in_nanoseconds = (NANOSECONDS_IN_A_SECOND / SAMPLE_RATE)
		* MIN_FFT_BLOCK_SIZE;

Vector g_powerOf2Vector; // Stores the correct number of powers of 2 to make the block calculations work (lol)

pthread_t thread;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition = PTHREAD_COND_INITIALIZER;

audioData* impulse;

/*
 * This buffer is used to store INCOMING audio from the mic.
 */
float *g_input_storage_buffer;

/*
 * This buffer is used to store OUTGOING audio that has been processed.
 */
float *g_output_storage_buffer1; // channel 1
float *g_output_storage_buffer2; // channel 2

void idleFunc();
void displayFunc();
void reshapeFunc(int width, int height);
void keyboardFunc(unsigned char, int, int);
void passiveMotionFunc(int, int);
void keyboardUpFunc(unsigned char, int, int);
void initialize_graphics();
void initialize_glut(int argc, char *argv[]);

void initializeGlobalParameters() {
	g_block_length = MIN_FFT_BLOCK_SIZE;
	g_num_blocks = g_impulse_length / g_block_length;
	g_max_factor = g_num_blocks / 4;
	g_input_storage_buffer_length = g_impulse_length / 4;
	g_output_storage_buffer1_length = g_input_storage_buffer_length * 2;
	g_output_storage_buffer2_length = g_input_storage_buffer_length * 2;
	g_end_sample = g_input_storage_buffer_length - 1;

	// Allocate memory for storage buffer, fill with 0s.
	g_input_storage_buffer = (float *) calloc(g_input_storage_buffer_length,
			sizeof(float));

	// Allocate memory for output storage buffer, fill with 0s.
	g_output_storage_buffer1 = (float *) calloc(g_output_storage_buffer1_length,
			sizeof(float));
	g_output_storage_buffer2 = (float *) calloc(g_output_storage_buffer2_length,
			sizeof(float));

}

//-----------------------------------------------------------------------------
// Name: initialize_glut( )
// Desc: Initializes Glut with the global vars
//-----------------------------------------------------------------------------
void initialize_glut(int argc, char *argv[]) {
	// initialize GLUT
	glutInit(&argc, argv);
	// double buffer, use rgb color, enable depth buffer
	glutInitDisplayMode( GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
	// initialize the window size
	glutInitWindowSize(g_width, g_height);
	// set the window postion
	glutInitWindowPosition(400, 100);
	// create the window
	glutCreateWindow("Aaron Dawson's NYU Thesis (2015)");
	// full screen
	if (g_fullscreen)
		glutFullScreen();

	// set the idle function - called when idle
	glutIdleFunc(idleFunc);
	// set the display function - called when redrawing
	glutDisplayFunc(displayFunc);
	// set the reshape function - called when client area changes
	glutReshapeFunc(reshapeFunc);
	// set the keyboard function - called on keyboard events
	glutKeyboardFunc(keyboardFunc);
	// get mouse position
	glutPassiveMotionFunc(passiveMotionFunc);

	glutKeyboardUpFunc(keyboardUpFunc);

	// do our own initialization
	initialize_graphics();
}

void idleFunc() {
	glutPostRedisplay();
}

void keyboardFunc(unsigned char key, int x, int y) {
	switch (key) {
	case 'f':
		if (!g_fullscreen) {
			g_last_width = g_width;
			g_last_height = g_height;
			glutFullScreen();
		} else {
			glutReshapeWindow(g_last_width, g_last_height);
		}
		g_fullscreen = !g_fullscreen;
		break;
	case 'q':
		exit(0);
		break;
	case 'a':
		a_pressed = true;
//		printf("'a' has been pressed.\n");
		break;
	case 'z':
		z_pressed = true;
		break;
	}
	glutPostRedisplay();
}

void passiveMotionFunc(int x, int y) {
	g_mouse_x = ((float) FFT_SIZE / 576) * (float) x
			- ((float) FFT_SIZE * 111 / 576);

	g_mouse_y = -4.14 + 8.28 * ((float) (g_height - y) / (float) g_height);

//	printf("x: %f, y: %f\n", g_mouse_x, g_mouse_y);
}

void keyboardUpFunc(unsigned char key, int x, int y) {
	switch (key) {
	case 'a':
		a_pressed = false;
//		printf("'a' has been released.\n");
		break;
	case 'z':
		z_pressed = false;
		break;
	}
	glutPostRedisplay();
}

void reshapeFunc(int w, int h) {
	// save the new window size
	g_width = w;
	g_height = h;
	// map the view port to the client area
	glViewport(0, 0, w, h);
	// set the matrix mode to project
	glMatrixMode( GL_PROJECTION);
	// load the identity matrix
	glLoadIdentity();
	// create the viewing frustum
	//gluPerspective( 45.0, (GLfloat) w / (GLfloat) h, .05, 50.0 );
	gluPerspective(45.0, (GLfloat) w / (GLfloat) h, 1.0, 1000.0);
	// set the matrix mode to modelview
	glMatrixMode( GL_MODELVIEW);
	// load the identity matrix
	glLoadIdentity();

	// position the view point
	//  void gluLookAt( GLdouble eyeX,
	//                 GLdouble eyeY,
	//                 GLdouble eyeZ,
	//                 GLdouble centerX,
	//                 GLdouble centerY,
	//                 GLdouble centerZ,
	//                 GLdouble upX,
	//                 GLdouble upY,
	//                 GLdouble upZ )

	gluLookAt(0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
	/*gluLookAt( 0.0f, 3.5f * sin( 0.0f ), 3.5f * cos( 0.0f ),
	 0.0f, 0.0f, 0.0f,
	 0.0f, 1.0f , 0.0f );*/
}

//-----------------------------------------------------------------------------
// Name: initialize_graphics( )
// Desc: sets initial OpenGL states and initializes any application data
//-----------------------------------------------------------------------------
void initialize_graphics() {
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);					// Black Background
	// set the shading model to 'smooth'
	glShadeModel( GL_SMOOTH);
	// enable depth
	glEnable( GL_DEPTH_TEST);
	// set the front faces of polygons
	glFrontFace( GL_CCW);
	// set fill mode
	glPolygonMode( GL_FRONT_AND_BACK, g_fillmode);
	// enable lighting
	glEnable( GL_LIGHTING);
	// enable lighting for front
	glLightModeli( GL_FRONT_AND_BACK, GL_TRUE);
	// material have diffuse and ambient lighting
	glColorMaterial( GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
	// enable color
	glEnable( GL_COLOR_MATERIAL);
	// normalize (for scaling)
	glEnable( GL_NORMALIZE);
	// line width
	glLineWidth(g_linewidth);

	// enable light 0
	glEnable( GL_LIGHT0);

	glEnable( GL_LIGHT1);

}

void displayFunc() {
	int i;

	float x_location;

//	while (!g_ready) {
//		usleep(1000);
//	}
//	g_ready = false;

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// draw view here
	if ((int) g_mouse_x >= 0 && (int) g_mouse_x < FFT_SIZE && a_pressed == true) {
		float value = g_mouse_y - g_height_top;
		if (value > 0.0f) {
			value = 0.0f;
		}
		if (value
				< (g_height_top
						- (g_height_bottom + bottom_vals[(int) g_mouse_x]))
						* -1) {
			value = (g_height_top
					- (g_height_bottom + bottom_vals[(int) g_mouse_x])) * -1;
		}
//		printf("g_mouse_y: %f, value: %f\n", g_mouse_y, value);
		top_vals[(int) g_mouse_x] = value;
	}

	if ((int) g_mouse_x >= 0 && (int) g_mouse_x < FFT_SIZE && z_pressed == true) {
		float value = g_mouse_y + g_height_bottom;
		if (value < (g_height_top - g_height_bottom) * -1) {
			value = (g_height_top - g_height_bottom) * -1;
		}
		if (value
				> (g_height_top + top_vals[(int) g_mouse_x] + g_height_bottom)) {
			value = g_height_top + top_vals[(int) g_mouse_x] + g_height_bottom;
		}

		value += (g_height_top - g_height_bottom);

//			printf("g_mouse_y: %f, g_height_bottom: %f, value: %f\n", g_mouse_y, g_height_bottom, value);
		bottom_vals[(int) g_mouse_x] = value;
	}

	glPushMatrix();
	{
		glBegin(GL_LINE_STRIP);
		for (i = 0; i < FFT_SIZE; i++) {
			x_location = (float) i * g_relative_width / FFT_SIZE
					- g_relative_width / 2;
			glVertex3f(x_location, g_height_top + top_vals[i], 0.0f);
		}
		glEnd();
	}
	glPopMatrix();

	glPushMatrix();
	{
		glBegin(GL_LINE_STRIP);
		for (i = 0; i < FFT_SIZE; i++) {
			x_location = (float) i * g_relative_width / FFT_SIZE
					- g_relative_width / 2;
			glVertex3f(x_location, g_height_bottom + bottom_vals[i], 0.0f);
		}
		glEnd();
	}
	glPopMatrix();

	glFlush();
	glutSwapBuffers();
}

/*
 * This function takes an FFT of a portion of audio from the g_input_storage_buffer,
 * zero-pads it to twice its length, multiplies it with a specific block of FFT
 * data from the impulse, takes the IFFT of the resulting data, and places that
 * data in the g_output_storage_buffer after the correct amount of callback cycles.
 */
void *calculateFFT(void *incomingFFTArgs) {

	FFTArgs *fftArgs = (FFTArgs *) incomingFFTArgs;

	int i;

	int numCyclesToWait = fftArgs->num_callbacks_to_complete - 1;

	int counter_target = (fftArgs->counter + numCyclesToWait)
			% (g_max_factor * 2);
	if (counter_target == 0) {
		counter_target = g_max_factor * 2;
	}

	pthread_detach(pthread_self());

	//	 1. Create buffer with length = 2 * (last_sample_index - first_sample_index),
	//	    fill the buffer with 0s.
	int blockLength = fftArgs->last_sample_index - fftArgs->first_sample_index
			+ 1;
	int convLength = blockLength * 2;

	int volumeFactor = blockLength / g_block_length; // 1, 2, 4, 8, etc

	complex *inputAudio = calloc(convLength, sizeof(complex));
	// 2. Take audio from g_input_storage_buffer (first_sample_index to last_sample_index)
	//    and place it into the buffer created in part 1 (0 to (last_sample_index - first_sample_index)).
	for (i = 0; i < blockLength; i++) {
		inputAudio[i].Re = g_input_storage_buffer[fftArgs->first_sample_index
				+ i];
	}

	//	printf(
	//			"Thread %d: Start convolving sample %d to %d with h%d. This process will take %d cycles and complete when N = %d.\n",
	//			pthread_self(), fftArgs->first_sample_index, fftArgs->last_sample_index,
	//			fftArgs->impulse_block_number, numCyclesToWait + 1, counter_target);

	// 3. Take the FFT of the buffer created in part 1.
	complex *temp = calloc(convLength, sizeof(complex));
	fft(inputAudio, convLength, temp);

	// 4. Determine correct impulse FFT block based in impulse_block_number. The length of this
	//	  block should automatically be the same length as the length of the buffer created in part 1
	//    that now holds the input audio data.
	int fftBlockNumber = fftArgs->impulse_block_number;

	// If the impulse is mono
	if (impulse->numChannels == MONO) {

		// 5. Create buffer of length 2 * (last_sample_index - first_sample_index) to hold the result of
		//    FFT multiplication.
		complex *convResult = calloc(convLength, sizeof(complex));
		// 6. Complex multiply the buffer created in part 1 with the impulse FFT block determined in part 4,
		//    and store the result in the buffer created in part 5.
		complex c;

		for (i = 0; i < convLength; i++) {
			c = complex_mult(inputAudio[i],
					g_fftData_ptr->fftBlocks1[fftBlockNumber][i]);

			convResult[i].Re = c.Re;
			convResult[i].Im = c.Im;

		}
		// 7. Take the IFFT of the buffer created in part 5.
		ifft(convResult, convLength, temp);

		// 8. When the appropriate number of callback cycles have passed (num_callbacks_to_complete), put
		//    the real values of the buffer created in part 5 into the g_output_storage_buffer
		//    (sample 0 through sample 2 * (last_sample_index - first_sample_index)
		while (g_counter != counter_target) {
			nanosleep((const struct timespec[] ) { {0,g_block_duration_in_nanoseconds/NUM_CHECKS_PER_CYCLE}}, NULL);
		}

		// Put data in output buffer
		for (i = 0; i < convLength; i++) {
			g_output_storage_buffer1[i] += convResult[i].Re / volumeFactor;
		}

		free(convResult);

	}

	// If the impulse is stereo
	if (impulse->numChannels == STEREO) {

		// 5. Create buffer of length 2 * (last_sample_index - first_sample_index) to hold the result of
		//    FFT multiplication.
		complex *convResultLeft = calloc(convLength, sizeof(complex));
		complex *convResultRight = calloc(convLength, sizeof(complex));

		// 6. Complex multiply the buffer created in part 1 with the impulse FFT block determined in part 4,
		//    and store the result in the buffer created in part 5.
		complex c1, c2;

		for (i = 0; i < convLength; i++) {
			// Left channel
			c1 = complex_mult(inputAudio[i],
					g_fftData_ptr->fftBlocks1[fftBlockNumber][i]);
			// Right channel
			c2 = complex_mult(inputAudio[i],
					g_fftData_ptr->fftBlocks2[fftBlockNumber][i]);

			convResultLeft[i].Re = c1.Re;
			convResultLeft[i].Im = c1.Im;

			convResultRight[i].Re = c2.Re;
			convResultRight[i].Im = c2.Im;

		}

		// 7. Take the IFFT of the buffer created in part 5.
		ifft(convResultLeft, convLength, temp);
		ifft(convResultRight, convLength, temp);

		// 8. When the appropriate number of callback cycles have passed (num_callbacks_to_complete), put
		//    the real values of the buffer created in part 5 into the g_output_storage_buffer
		//    (sample 0 through sample 2 * (last_sample_index - first_sample_index)
		while (g_counter != counter_target) {
			nanosleep((const struct timespec[] ) { {0,g_block_duration_in_nanoseconds/NUM_CHECKS_PER_CYCLE}}, NULL);
		}

		// Put data in output buffer
		for (i = 0; i < convLength; i++) {
			g_output_storage_buffer1[i] += convResultLeft[i].Re / volumeFactor; // left channel
			g_output_storage_buffer2[i] += convResultRight[i].Re / volumeFactor; // right channel
		}

		free(convResultLeft);
		free(convResultRight);

	}

//	printf(
//			"Thread %d: The result of the convolution of sample %d to %d with h%d has been added to the output buffer. Expected arrival: when n = %d.\n",
//			pthread_self(), fftArgs->first_sample_index, fftArgs->last_sample_index,
//			fftArgs->impulse_block_number, counter_target);

	// Free remaining buffers
	free(temp);
	free(inputAudio);

	free(fftArgs);
	pthread_exit(NULL);
	return NULL;
}

/*
 *  Description:  Callback for Port Audio
 */
static int paCallback(const void *inputBuffer, void *outputBuffer,
		unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo,
		PaStreamCallbackFlags statusFlags, void *userData) {

	float *inBuf = (float*) inputBuffer;
	float *outBuf = (float*) outputBuffer;

	int i, j;

	if (impulse->numChannels == MONO) {
		for (i = 0; i < framesPerBuffer; i++) {
			outBuf[i] = g_output_storage_buffer1[i];
		}
	}

	if (impulse->numChannels == STEREO) {
		for (i = 0; i < framesPerBuffer; i++) {
			outBuf[2*i] = g_output_storage_buffer1[i];
			outBuf[2*i + 1] = g_output_storage_buffer2[i];
		}
	}

	++g_counter;

	if (g_counter >= g_max_factor * 2 + 1) {
		g_counter = 1;
	}

	// Shift g_input_storage_buffer to the left by g_block_length
	for (i = 0; i < g_input_storage_buffer_length - g_block_length; i++) {
		g_input_storage_buffer[i] = g_input_storage_buffer[i + g_block_length];
	}

	// Fill right-most portion of g_input_storage_buffer with most recent audio
	for (i = 0; i < g_block_length; i++) {
		g_input_storage_buffer[g_input_storage_buffer_length - g_block_length
				+ i] = inBuf[i] * 0.00001f;
	}

	/*
	 * Create threads
	 */
	for (j = 0; j < g_powerOf2Vector.size; j++) {
		int factor = vector_get(&g_powerOf2Vector, j);
		if (g_counter % factor == 0 && g_counter != 0) {

			/*
			 * Take the specified samples from the input_storage_buffer, zero-pad them to twice their
			 * length, FFT them, multiply the resulting spectrum by the corresponding impulse FFT block,
			 * IFFT the result, put the result in the output_storage_buffer.
			 */
			FFTArgs *fftArgs = (FFTArgs *) malloc(sizeof(FFTArgs));

			fftArgs->first_sample_index = (1 + g_end_sample
					- g_block_length * factor);
			fftArgs->last_sample_index = g_end_sample;
			fftArgs->impulse_block_number = (j * 2 + 1);
			fftArgs->num_callbacks_to_complete = factor;
			fftArgs->counter = g_counter;
			pthread_create(&thread, NULL, calculateFFT, (void *) fftArgs);

			FFTArgs *fftArgs2 = (FFTArgs *) malloc(sizeof(FFTArgs));
			fftArgs2->first_sample_index = (1 + g_end_sample
					- g_block_length * factor);
			fftArgs2->last_sample_index = g_end_sample;
			fftArgs2->impulse_block_number = (j * 2 + 2);
			fftArgs2->num_callbacks_to_complete = factor * 2;
			fftArgs2->counter = g_counter;

			pthread_create(&thread, NULL, calculateFFT, (void *) fftArgs2);

		}
	}

	// Shift g_output_storage_buffer
	for (i = 0; i < g_output_storage_buffer1_length - g_block_length; i++) {
		g_output_storage_buffer1[i] = g_output_storage_buffer1[i
				+ g_block_length];
		g_output_storage_buffer2[i] = g_output_storage_buffer2[i
				+ g_block_length];
	}

	return paContinue;
}

void runPortAudio() {
	PaStream* stream;
	PaStreamParameters outputParameters;
	PaStreamParameters inputParameters;
	PaError err;
	/* Initialize PortAudio */
	Pa_Initialize();
	/* Set output stream parameters */
	outputParameters.device = Pa_GetDefaultOutputDevice();
	outputParameters.channelCount = impulse->numChannels;
	outputParameters.sampleFormat = paFloat32;
	outputParameters.suggestedLatency = Pa_GetDeviceInfo(
			outputParameters.device)->defaultLowOutputLatency;
	outputParameters.hostApiSpecificStreamInfo = NULL;
	/* Set input stream parameters */
	inputParameters.device = Pa_GetDefaultInputDevice();
	inputParameters.channelCount = MONO;
	inputParameters.sampleFormat = paFloat32;
	inputParameters.suggestedLatency =
			Pa_GetDeviceInfo(inputParameters.device)->defaultLowInputLatency;
	inputParameters.hostApiSpecificStreamInfo = NULL;
	/* Open audio stream */
	err = Pa_OpenStream(&stream, &inputParameters, &outputParameters,
	SAMPLE_RATE, MIN_FFT_BLOCK_SIZE, paNoFlag, paCallback, NULL);

	if (err != paNoError) {
		printf("PortAudio error: open stream: %s\n", Pa_GetErrorText(err));
	}
	/* Start audio stream */
	err = Pa_StartStream(stream);
	if (err != paNoError) {
		printf("PortAudio error: start stream: %s\n", Pa_GetErrorText(err));
	}

	glutMainLoop();

	/* Get user input */
	char ch = '0';
	while (ch != 'q') {
		printf("Press 'q' to finish execution: ");
		ch = getchar();
	}

	err = Pa_StopStream(stream);
	/* Stop audio stream */
	if (err != paNoError) {
		printf("PortAudio error: stop stream: %s\n", Pa_GetErrorText(err));
	}
	/* Close audio stream */
	err = Pa_CloseStream(stream);
	if (err != paNoError) {
		printf("PortAudio error: close stream: %s\n", Pa_GetErrorText(err));
	}
	/* Terminate audio stream */
	err = Pa_Terminate();
	if (err != paNoError) {
		printf("PortAudio error: terminate: %s\n", Pa_GetErrorText(err));
	}
}

void loadImpulse() {

	impulse = fileToBuffer("churchIR.wav");
	impulse = zeroPadToNextPowerOfTwo(impulse);
	g_impulse_length = impulse->numFrames;
	Vector blockLengthVector = determineBlockLengths(impulse);
	BlockData* data_ptr = allocateBlockBuffers(blockLengthVector, impulse);
	partitionImpulseIntoBlocks(blockLengthVector, data_ptr, impulse);
	g_fftData_ptr = allocateFFTBuffers(data_ptr, blockLengthVector, impulse);
}

void initializePowerOf2Vector() {
	vector_init(&g_powerOf2Vector);
	int counter = 0;
	while (pow(2, counter) <= g_max_factor) {
		vector_append(&g_powerOf2Vector, pow(2, counter++));
	}
}

int main(int argc, char **argv) {

//	printf("Block duration in nanoseconds: %lu\n",
//			g_block_duration_in_nanoseconds);
	int i;
	for (i = 0; i < FFT_SIZE; i++) {
			top_vals[i] = 0.0f;
			bottom_vals[i] = 0.0f;
		}

	loadImpulse();

	initialize_glut(argc, argv);
//
	initializeGlobalParameters();
//
	initializePowerOf2Vector();
//
	runPortAudio();

	return 0;
}

