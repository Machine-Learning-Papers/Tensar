// g++ -std=c++11 -stdlib=libc++ NeuralNetwork.cpp -o NeuralNetwork -isysroot /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.13.sdk -Wl,-search_paths_first -Wl,-headerpad_max_install_names -framework OpenGL -framework OpenGL -framework GLUT -framework Cocoa -Wno-deprecated

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <pthread.h>
#include <algorithm>
#include <vector>
#include <cmath>
#include <mutex>

#ifdef __APPLE__
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif

#define WIDTH 800
#define HEIGHT 600

#define LEARNING_RATE 0.01
#define MOMENTUM 0.6
#define WEIGHT_DECAY 0.001

#define BMP_HEADER_SIZE 54

using namespace std;

struct Point2d {
        int x;
        int y;

        Point2d(int x, int y) {
                this->x = x;
                this->y = y;
        }

        Point2d(const Point2d& p) {
                this->x = p.x;
                this->y = p.y;
        }

        Point2d& operator = (const Point2d& p) {
                this->x = p.x;
                this->y = p.y;
                return *this;
        }

};

namespace NeuralNetwork {

class InputCase;
class Layer;

enum LayerType { convolutional, fc, relu, pool, dropout_layer };

vector<Point2d> points;
int timebase_timestamp = 0;
int frame_counter = 0;
char current_fps_buffer[20];
unsigned char *producer_frame_buffer = NULL;
unsigned char *consumer_frame_buffer = NULL;
mutex consumer_mutex;
bool is_consuming_frame_buffer = false;

vector<Layer*> layers;

struct point_t
{
        int x, y, z;
};

struct size_t
{
        int width;
        int height;
        int depth;
};

struct gradient_t
{
        float grad;
        float oldgrad;
        gradient_t()
        {
                grad = 0;
                oldgrad = 0;
        }
};

struct range_t
{
        int min_x, min_y, min_z;
        int max_x, max_y, max_z;
};

static float update_weight( float w, gradient_t& grad, float multp = 1 )
{
        float m = (grad.grad + grad.oldgrad * MOMENTUM);
        w -= LEARNING_RATE  * m * multp +
             LEARNING_RATE * WEIGHT_DECAY * w;
        return w;
}

static void update_gradient( gradient_t& grad )
{
        grad.oldgrad = (grad.grad + grad.oldgrad * MOMENTUM);
}

/* BEGIN - VISUALS DATA STRUCTURES */
class TensorRenderFrameBuffer {

public:

int width; // pixels
int height; // pixels
char *caption = NULL;

unsigned char *producer_frame_buffer = NULL;
unsigned char *consumer_frame_buffer = NULL;
mutex consumer_mutex;
bool is_consuming_frame_buffer = false;

TensorRenderFrameBuffer(int _width, int _height) {
        producer_frame_buffer = (unsigned char *)calloc((_width * _height * 3), sizeof(unsigned char));
        consumer_frame_buffer = (unsigned char *)calloc((_width * _height * 3), sizeof(unsigned char));
        width = _width;
        height = _height;
}

void set(int x, int y, unsigned char value) const
{
        assert(x >= 0 && y >= 0);
        assert(x < width && y < height);
        producer_frame_buffer[(y * width * 3) + x * 3] = 0;
        producer_frame_buffer[(y * width * 3) + x * 3 + 1] = value; // green
        producer_frame_buffer[(y * width * 3) + x * 3 + 2] = 0;
}

void swapBuffers()
{
        if(!is_consuming_frame_buffer) {
                consumer_mutex.lock();
                unsigned char *tmp = producer_frame_buffer;
                producer_frame_buffer = consumer_frame_buffer;
                consumer_frame_buffer = tmp;
                consumer_mutex.unlock();
        }
}

~TensorRenderFrameBuffer() {
        if(producer_frame_buffer != NULL) {
                delete[] producer_frame_buffer;
        }

        if(consumer_frame_buffer != NULL) {
                delete[] consumer_frame_buffer;
        }
}

};


class LayerGridFrameBuffer {

public:

int width;
int height;
char **header_titles = NULL;
TensorRenderFrameBuffer **cells = NULL;

LayerGridFrameBuffer() {

}

LayerGridFrameBuffer(int _width, int _height) {
        // Creates an array of pointers
        cells = new TensorRenderFrameBuffer*[_width * _height];

        // Initialize the array with null pointers
        for(int i=0; i<_width * _height; i++)
                cells[i] = NULL;
        width = _width;
        height = _height;
}

TensorRenderFrameBuffer* operator()(int x, int y) const
{
        return this->get(x, y);
}

TensorRenderFrameBuffer* get(int x, int y) const
{
        assert(x >= 0 && y >= 0);
        assert(x < width && y < height);
        return cells[(y * width) + x];
}

void set(int x, int y, TensorRenderFrameBuffer* cell) const
{
        assert(x >= 0 && y >= 0);
        assert(x < width && y < height);
        cells[(y * width) + x] = cell;
}

~LayerGridFrameBuffer() {
        if(cells != NULL) {
                for(int i=0; i<width*height; i++)
                        if(cells[i] != NULL)
                                delete cells[i];
                delete[] cells;
        }
}

};
/* END - VISUALS DATA STRUCTURES */

class Tensor {

public:

size_t size;

~Tensor() {
}

};

class TensorFloat : public Tensor {

public:

float *values = NULL;

TensorFloat() {

}

TensorFloat(int width, int height, int depth) {
        values = new float[width * height * depth];
        size.width = width;
        size.height = height;
        size.depth = depth;
}

TensorFloat(const TensorFloat& t) {
        values = new float[t.size.width * t.size.height * t.size.depth];
        memcpy(this->values, t.values, t.size.width * t.size.height * t.size.depth * sizeof(float));
        this->size = t.size;
}

float& operator()( int x, int y, int z ) const
{
        return this->get( x, y, z );
}

float& get( int x, int y, int z ) const
{
        assert(x >= 0 && y >= 0 && z >= 0);
        assert(x < size.width && y < size.height && z < size.depth);
        return values[z * (size.width * size.height) + y * size.width + x];
}

~TensorFloat() {
        if(values != NULL) {
                delete[] values;
        }
}

};

class TensorGradient : public Tensor {

public:

gradient_t *values;

TensorGradient(int width, int height, int depth) {
        values = new gradient_t[width * height * depth];
        size.width = width;
        size.height = height;
        size.depth = depth;
}

TensorGradient(const TensorGradient& t) {
        values = new gradient_t[t.size.width * t.size.height * t.size.depth];
        memcpy(this->values, t.values, t.size.width * t.size.height * t.size.depth * sizeof(float));
        this->size = t.size;
}

gradient_t& operator()( int x, int y, int z )
{
        return this->get( x, y, z );
}

gradient_t& get( int x, int y, int z )
{
        assert(x >= 0 && y >= 0 && z >= 0);
        assert(x < size.width && y < size.height && z < size.depth);
        return values[z * (size.width * size.height) + y * size.width * x];
}

~TensorGradient() {
        delete[] values;
}
};

// Layer abstract class
class Layer {

public:

TensorFloat *output;
LayerType type;
LayerGridFrameBuffer *gridRenderFrameBuffer;

virtual void activate(TensorFloat*)=0;
virtual void activate()=0;

};

class ConvolutionalLayer : public Layer {

public:

TensorFloat *input_gradients, *input;
vector<TensorFloat> filters;
vector<TensorGradient> filter_gradients;
int stride, extend_filter;

ConvolutionalLayer(int stride, int extend_filter, int number_filters, size_t in_size) {
        type = LayerType::convolutional;

        //Define and alloc a grid layout for render buffers

        // {input, filter, output}
        // {cellObj, cellObj, cellObj}
        // {cellObj, cellObj, cellObj}
        // ...
        // {cellObj, cellObj, cellObj}

        gridRenderFrameBuffer = new LayerGridFrameBuffer(3, number_filters); // 3 = {input, filter, output}

        // Initialize first column of the grid with Inputs buffers
        for(int i=0; i<number_filters; i++) {
                gridRenderFrameBuffer->set(0, i, new TensorRenderFrameBuffer(in_size.width, in_size.height));
        }

        // Initialize second column of the grid with Filters buffers
        for(int i=0; i<number_filters; i++) {
                gridRenderFrameBuffer->set(1, i, new TensorRenderFrameBuffer(extend_filter, extend_filter));
        }

        // Initialize third column of the grid with Output buffers
        for(int i=0; i<number_filters; i++) {
                gridRenderFrameBuffer->set(2, i, new TensorRenderFrameBuffer((in_size.width - extend_filter) / stride + 1, (in_size.height - extend_filter) / stride + 1));
        }

        input_gradients = new TensorFloat(in_size.width, in_size.height, in_size.depth);
        input = new TensorFloat(in_size.width, in_size.height, in_size.depth);
        output = new TensorFloat((in_size.width - extend_filter) / stride + 1, (in_size.height - extend_filter) / stride + 1, number_filters);
        this->stride = stride;
        this->extend_filter = extend_filter;
        assert( (float( in_size.width - extend_filter ) / stride + 1) == ((in_size.width - extend_filter) / stride + 1) );
        assert( (float( in_size.height - extend_filter ) / stride + 1) == ((in_size.height - extend_filter) / stride + 1) );

        TensorRenderFrameBuffer* filterFrameBuffer;

        for(int a = 0; a < number_filters; a++) {
                filterFrameBuffer = gridRenderFrameBuffer->get(1, a);
                TensorFloat t(extend_filter, extend_filter, in_size.depth);
                int maxval = extend_filter * extend_filter * in_size.depth;

                for(int x = 0; x < extend_filter; x++)
                {
                        for(int y = 0; y < extend_filter; y++)
                        {
                                for(int z = 0; z < in_size.depth; z++)
                                {
                                        float value = 1.0f / maxval * rand() / float( RAND_MAX );
                                        t(x, y, z) = value;
                                        filterFrameBuffer->set(x, y, (unsigned int)(value * 255));
                                }
                        }
                }

                filters.push_back(t);
                filterFrameBuffer->swapBuffers();
        }

        for(int i = 0; i < number_filters; i++) {
                TensorGradient t(extend_filter, extend_filter, in_size.depth);
                filter_gradients.push_back( t );
        }
}

point_t map_to_input(point_t out, int z) {
        out.x *= stride;
        out.y *= stride;
        out.z = z;
        return out;
}

int normalize_range(float f, int max, bool lim_min) {
        if(f <= 0) { return 0; }

        max -= 1;

        if(f >= max) { return max; }

        if(lim_min) {
                // left side of inequality
                return ceil( f );
        } else {
                return floor( f );
        }
}

range_t map_to_output(int x, int y) {
        float a = x;
        float b = y;
        return {
                       normalize_range( (a - extend_filter + 1) / stride, output->size.width, true ),
                       normalize_range( (b - extend_filter + 1) / stride, output->size.height, true ),
                       0,
                       normalize_range( a / stride, output->size.width, false ),
                       normalize_range( b / stride, output->size.height, false ),
                       (int)filters.size() - 1,
        };
}

void activate(TensorFloat *in) {
        this->input = in;
        activate();
}

void activate() {

        for(int filter = 0; filter < filters.size(); filter++)
        {
                TensorFloat& filter_data = filters[filter];
                for(int x = 0; x < output->size.width; x++)
                {
                        for(int y = 0; y < output->size.height; y++)
                        {
                                point_t mapped = map_to_input( { (uint16_t)x, (uint16_t)y, 0 }, 0 );
                                float sum = 0;
                                for(int i = 0; i < extend_filter; i++)
                                {
                                        for(int j = 0; j < extend_filter; j++)
                                        {
                                                for(int z = 0; z < input->size.depth; z++)
                                                {
                                                        float f = filter_data( i, j, z );
                                                        float v = (*input)( mapped.x + i, mapped.y + j, z );
                                                        sum += f*v;
                                                }
                                        }
                                }
                                (*output)(x, y, filter) = sum;
                        }
                }
        }

}

void fix_weights() {

        for(int a = 0; a < filters.size(); a++)
        {
                for(int i = 0; i < extend_filter; i++)
                {
                        for(int j = 0; j < extend_filter; j++)
                        {
                                for(int z = 0; z < input->size.depth; z++)
                                {
                                        float& w = filters[a].get(i, j, z);
                                        TensorGradient tensor_gradient = filter_gradients[a];
                                        gradient_t& grad = tensor_gradient.get(i, j, z);
                                        w = update_weight(w, grad);
                                        update_gradient(grad);
                                }
                        }
                }
        }

}

void calc_grads(TensorFloat& grad_next_layer) {

        for ( int k = 0; k < filter_gradients.size(); k++ )
                for ( int i = 0; i < extend_filter; i++ )
                        for ( int j = 0; j < extend_filter; j++ )
                                for ( int z = 0; z < input->size.depth; z++ )
                                        filter_gradients[k].get( i, j, z ).grad = 0;

        for ( int x = 0; x < input->size.width; x++ )
        {
                for ( int y = 0; y < input->size.height; y++ )
                {
                        range_t rn = map_to_output( x, y );
                        for ( int z = 0; z < input->size.depth; z++ )
                        {
                                float sum_error = 0;
                                for ( int i = rn.min_x; i <= rn.max_x; i++ )
                                {
                                        int minx = i * stride;
                                        for ( int j = rn.min_y; j <= rn.max_y; j++ )
                                        {
                                                int miny = j * stride;
                                                for ( int k = rn.min_z; k <= rn.max_z; k++ )
                                                {
                                                        int w_applied = filters[k].get( x - minx, y - miny, z );
                                                        sum_error += w_applied * grad_next_layer( i, j, k );
                                                        filter_gradients[k].get( x - minx, y - miny, z ).grad += (*input)( x, y, z ) * grad_next_layer( i, j, k );
                                                }
                                        }
                                }
                                (*input_gradients)(x, y, z) = sum_error;
                        }
                }
        }
}

~ConvolutionalLayer() {
        delete gridRenderFrameBuffer;
}

};

class Network {
vector<Layer*> layers;

public:

Network() {

}

};

class InputCase {

public:

TensorFloat *data;
TensorFloat *out;

InputCase(size_t data_size, size_t out_size) {
        data = new TensorFloat(data_size.width, data_size.height, data_size.depth);
        out = new TensorFloat(out_size.width, out_size.height, out_size.depth);
}

~InputCase() {
        delete data;
        delete out;
}

};


}

/***********************************/


using namespace NeuralNetwork;

struct point_t
{
        int x, y, z;
};

/*
   static void print_tensor( tensor_t<float>& data )
   {
   int mx = data.size.x;
   int my = data.size.y;
   int mz = data.size.z;

   for ( int z = 0; z < mz; z++ )
   {
    printf( "[Dim%d]\n", z );
    for ( int y = 0; y < my; y++ )
    {
      for ( int x = 0; x < mx; x++ )
      {
        printf( "%.2f \t", (float)data.get( x, y, z ) );
      }
      printf( "\n" );
    }
   }
   }

   static tensor_t<float> to_tensor( std::vector<std::vector<std::vector<float>>> data )
   {
   int z = data.size();
   int y = data[0].size();
   int x = data[0][0].size();


   tensor_t<float> t( x, y, z );

   for ( int i = 0; i < x; i++ )
    for ( int j = 0; j < y; j++ )
      for ( int k = 0; k < z; k++ )
        t( i, j, k ) = data[k][j][i];
   return t;
   }
 */

uint32_t byteswap_uint32(uint32_t a)
{
        return ((((a >> 24) & 0xff) << 0) |
                (((a >> 16) & 0xff) << 8) |
                (((a >> 8) & 0xff) << 16) |
                (((a >> 0) & 0xff) << 24));
}

uint8_t* read_file( const char* szFile )
{
        ifstream file( szFile, ios::binary | ios::ate );
        streamsize size = file.tellg();
        file.seekg( 0, ios::beg );

        if ( size == -1 )
                return nullptr;

        uint8_t* buffer = new uint8_t[size];
        file.read( (char*)buffer, size );
        return buffer;
}

void drawString(int x, int y, char* msg) {
        glColor3d(0.0, 0.0, 0.0);
        glRasterPos2d(x, HEIGHT - y);
        for (const char *c = msg; *c != '\0'; c++) {
                glutBitmapCharacter(GLUT_BITMAP_HELVETICA_10, *c);
        }
}

GLuint LoadTexture()
{
        if(consumer_frame_buffer == NULL) {
                return NULL;
        }

        GLuint texture;
        int width, height;

        width = 28;
        height = 28;

        glGenTextures( 1, &texture );
        glBindTexture( GL_TEXTURE_2D, texture );
        glTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE,GL_MODULATE );
        glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_NEAREST );
        glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,GL_LINEAR );
        glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,GL_REPEAT );
        glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,GL_REPEAT );
        gluBuild2DMipmaps( GL_TEXTURE_2D, GL_RGB, width, height, GL_RGB, GL_UNSIGNED_BYTE, consumer_frame_buffer);

        return texture;
}

void display(void)
{
        frame_counter++;

        glClear(GL_COLOR_BUFFER_BIT);
        for (auto point : points) {
                glPointSize(4.0);
                glColor3f(0.0, 0.0, 1.0);
                glBegin(GL_POINTS);
                glVertex2f(point.x, HEIGHT - point.y);
                glEnd();
        }

        if(glutGet(GLUT_ELAPSED_TIME) - timebase_timestamp > 1000) {
                int current_timestamp = glutGet(GLUT_ELAPSED_TIME);
                snprintf(current_fps_buffer, 20, "%4.1ffps", (frame_counter * 1000.0)/(current_timestamp - timebase_timestamp));
                timebase_timestamp = current_timestamp;
                frame_counter = 0;
        }

        glColor3f(1,1,1);

        for(Layer* layer: layers){
          LayerGridFrameBuffer *grid = layer->gridRenderFrameBuffer;

          for(int x=0; x<grid->width; x++)
            for(int y=0; y<grid->height; y++) {
              cout << "Rendering buffer\n";
              TensorRenderFrameBuffer* tensorFrameBuffer = grid->get(x, y);
            }

        }


        consumer_mutex.lock();
        is_consuming_frame_buffer = true;
        GLuint texture = LoadTexture();
        is_consuming_frame_buffer = false;
        consumer_mutex.unlock();

        if(texture != NULL) {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, texture);
                glBegin(GL_QUADS);
                glTexCoord2f(0, 0);
                glVertex2f(0, HEIGHT - 0);
                glTexCoord2f(0, 1);
                glVertex2f(0, HEIGHT - 100);
                glTexCoord2f(1, 1);
                glVertex2f(100, HEIGHT - 100);
                glTexCoord2f(1, 0);
                glVertex2f(100, HEIGHT - 0);
                glEnd();
                glDisable(GL_TEXTURE_2D);
        }

        drawString(WIDTH - 45, 15, current_fps_buffer);

        glFlush();
        glutSwapBuffers();
}

vector<InputCase*> read_test_cases()
{
        vector<InputCase*> cases;

        producer_frame_buffer = (unsigned char *)malloc((28 * 28 * 3));
        consumer_frame_buffer = (unsigned char *)malloc((28 * 28 * 3));

        uint8_t* train_image = read_file( "train-images.idx3-ubyte" );
        uint8_t* train_labels = read_file( "train-labels.idx1-ubyte" );
        uint32_t case_count = byteswap_uint32( *(uint32_t*)(train_image + 4) );

        for(int i = 0; i < case_count; i++)
        {

                NeuralNetwork::size_t input_size{28, 28, 1};
                NeuralNetwork::size_t out_size{10, 1, 1};

                InputCase *c = new InputCase(input_size, out_size);

                uint8_t* img = train_image + 16 + i * (28 * 28);
                uint8_t* label = train_labels + 8 + i;

                for ( int x = 0; x < 28; x++ )
                        for ( int y = 0; y < 28; y++ ) {
                                (*c->data)(x, y, 0) = img[x + y * 28] / 255.f;
                                producer_frame_buffer[x * 3 + y * 28 * 3] = 0;
                                producer_frame_buffer[x * 3 + y * 28 * 3 + 1] = img[x + y * 28]; // green byte
                                producer_frame_buffer[x * 3 + y * 28 * 3 + 2] = 0;
                        }

                if(!is_consuming_frame_buffer) {
                        consumer_mutex.lock();
                        unsigned char *tmp = producer_frame_buffer;
                        producer_frame_buffer = consumer_frame_buffer;
                        consumer_frame_buffer = tmp;
                        consumer_mutex.unlock();
                }

                for ( int b = 0; b < 10; b++ ) {
                        (*c->out)(b, 0, 0) = *label == b ? 1.0f : 0.0f;
                }

                cases.push_back(c);
        }

        delete[] train_image;
        delete[] train_labels;

        return cases;
}

void idle(void)
{
        glutPostRedisplay();
}

void reshape(int w, int h)
{
        glViewport(0, 0, WIDTH, HEIGHT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluOrtho2D(0.0, (GLdouble) w, 0.0, (GLdouble) h);
}

float train(vector<Layer*> &layers, InputCase *input_case)//TensorFloat *data, TensorFloat *expected)
{
        for(int i = 0; i < layers.size(); i++)
        {
                Layer *layer = layers[i];

                if(i == 0) { layer->activate(input_case->data); }
                else       { layer->activate(layers[i - 1]->output); }
        }

        return 0.0f;
/*
   tensor_t<float> grads = layers.back()->out - expected;

   for ( int i = layers.size() - 1; i >= 0; i-- )
   {
    if ( i == layers.size() - 1 )
      calc_grads( layers[i], grads );
    else
      calc_grads( layers[i], layers[i + 1]->grads_in );
   }

   for ( int i = 0; i < layers.size(); i++ )
   {
    fix_weights( layers[i] );
   }

   float err = 0;
   for ( int i = 0; i < grads.size.x * grads.size.y * grads.size.z; i++ )
   {
    float f = expected.data[i];
    if ( f > 0.5 )
      err += abs(grads.data[i]);
   }
   return err * 100;
 */
}

static void* tensarThreadFunc(void* v) {
        vector<InputCase*> cases = read_test_cases();

        // stride, filter_width(extend_filter), num_filters, filter_size
        ConvolutionalLayer *cnn_layer = new ConvolutionalLayer(1, 5, 8, cases[0]->data->size);    // 28 * 28 * 1 -> 24 * 24 * 8

        layers.push_back(cnn_layer);

        for(int i=0; i<cases.size(); i++)
        {
                InputCase *input_case = cases[i];
                float xerr = train(layers, input_case);
        }
}

int main(int argc, char *argv[]) {

        //Network *network = new Network();

        points.push_back(Point2d(20,20));
        points.push_back(Point2d(30,30));
        points.push_back(Point2d(40,40));
        points.push_back(Point2d(50,50));
        points.push_back(Point2d(60,60));

        pthread_t tensarThreadId;
        pthread_create(&tensarThreadId, NULL, tensarThreadFunc, 0);

        glutInit(&argc, argv);
        glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
        glutInitWindowSize(WIDTH, HEIGHT);
        glutInitWindowPosition(0, 0);
        glutCreateWindow("Tensar");
        glutDisplayFunc(display);
        glutReshapeFunc(reshape);
        glutIdleFunc(idle);

        glClearColor(1.0, 1.0, 1.0, 1.0);
        glEnable(GL_LINE_SMOOTH);

        glutMainLoop();

        return 0;


//	ConvolutionalLayer *layer1 = new ConvolutionalLayer(1, 5, 8, cases[0].data.size);		// 28 * 28 * 1 -> 24 * 24 * 8
//relu_layer_t * layer2 = new relu_layer_t( layer1->out.size );
//pool_layer_t * layer3 = new pool_layer_t( 2, 2, layer2->out.size );				// 24 * 24 * 8 -> 12 * 12 * 8
//fc_layer_t * layer4 = new fc_layer_t(layer3->out.size, 10);					// 4 * 4 * 16 -> 10

//	layers.push_back( (Layer*)layer1 );
//layers.push_back( (layer_t*)layer2 );
//layers.push_back( (layer_t*)layer3 );
//layers.push_back( (layer_t*)layer4 );
}
