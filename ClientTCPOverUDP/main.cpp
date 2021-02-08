/* 
 * File:   main.cpp
 * Author: sk
 *
 * Created on October 9, 2020, 1:56 PM
 */

#include <cstdlib>
#include <stdio.h> 
#include <stdlib.h> 
#include <unistd.h> 
#include <string.h> 
#include <sys/types.h> 
#include <sys/socket.h> 
#include <arpa/inet.h> 
#include <netinet/in.h>
#include <pthread.h>
#include <vector>
#include <atomic>
#include <string>
#include <math.h>
#include "include/rand_generate.h"
#include "include/util/util_string.h"

extern "C"
{
    #include "include/util/buf.h"
    #include "include/crypt/crc32/crc32.h"
    #include "include/textcolor.h"
}

#include "include/util/lock.h"
#include "include/global.h"

using namespace std;

#define DEBUG_PRINT 1

#define SERVER_HOST  "127.0.0.1"
#define SERVER_PORT  25698 
#define UDP_BUFLEN   1024*128 

#define INTERVAL_SEND_DATA_MS      1000
#define LIMIT_COUNT_SEND           5
#define LIMIT_SIZE_INPUT_REGISTER  1000

#define TYPE_SEND_DEFAULT     0
#define TYPE_SEND_NO_WAIT     1

#define SEND_DATA_TYPE_CALBACK_DATA___CHECK_MTU       0x01
#define SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE   0x02

#define STATUS_DOWNLOAD_FILE__WAIT_START               0x00
#define STATUS_DOWNLOAD_FILE__NONE                     0x01
#define STATUS_DOWNLOAD_FILE__CHECK_MTU                0x02
#define STATUS_DOWNLOAD_FILE__RUN_UPLOAD               0x03
#define STATUS_DOWNLOAD_FILE__ERROR_NEED_REMOVE_TASK   0x04
#define STATUS_DOWNLOAD_FILE__END_UPLOAD_REMOVE_TASK   0x05

#define INTERVAL_CHECK_ACCOUNT_IN_SEC                 3 * 24 * 60 * 60
#define INTERVAL_DELETE_ITEM_CACHED_UID_TOKEN         60 * 2
#define UPLOAD_TASK_WAIT_IDLE_DELETE_IN_SEC           20

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

uint8_t default_xor_key[]  = { 0x10, 0xaf, 0x25, 0x36, 0xa8, 0xad, 0xd0, 0x13, 0x55, 0x77, 0x12, 0x08, 0x19, 0xdf, 0x3f, 0x3f };
uint8_t iv[]               = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
uint8_t iv2[]              = { 0x10, 0x51, 0x72, 0xb3, 0xf4, 0x80, 0xac, 0x53, 0x42, 0x35, 0x25, 0x2b, 0x7c, 0xbd, 0xbe, 0x18 };

struct ConnectionInfo 
{
    int sockfd;
    struct sockaddr_in remote_addr;
    short rand_value;
    bool init;
    string address_and_port;
    
    ConnectionInfo() 
    {
        init              = false;
        address_and_port  = "";
        rand_value        = myrandom(5000);
    }
};

//-----------------------------------------------------------------------------------------
struct ReadCommand
{
    ConnectionInfo *ci;
    
    bool init_ok;
    uint64_t timestamp;
    long len_message;
    string cmd;
    vector<string> params;
    int len_buffer;
    unsigned char* buffer;

    ReadCommand()
    {
        ci  = NULL;
        cmd = "";
        len_message = 0;
        init_ok = false;

        len_buffer = 0;
        buffer = NULL;
    }

    ~ReadCommand()
    {
        if(buffer != NULL)
        {
            free(buffer);
            buffer = NULL;
        }
    }
};
///----------------------------------------------------------------------------------------
//-----------------------------------------------------------------------------------------
struct CheckMTU_STEP
{
    uint8_t count_iteration, counter_test;
    uint32_t test, lask_k, diff;
    vector<string> params;
    
    CheckMTU_STEP(CheckMTU_STEP *data)
    {
        count_iteration = data->count_iteration;
        test            = data->test;
        lask_k          = data->lask_k;
        diff            = data->diff;
        counter_test    = data->counter_test;
    }
    
    CheckMTU_STEP()
    {
        count_iteration = 1;
        test            = 1024 * 56;
        lask_k          = 0;
        diff            = 0;
        counter_test    = 0;
    }
};
//-----------------------------------------------------------------------------------------
struct UploadFile
{
    time_t time_created;
    
    // ++ UDP info ---------------
    int soket;
    struct sockaddr_in si_other;
    socklen_t slen;
    string address_and_port;
    // -- UDP info ---------------
    
    uint8_t status;             // статус работы 
    string fullFilePath;
    FILE *file;
    pthread_rwlock_t lock_item;
    vector<string> params;
    
    CheckMTU_STEP *mtu;
    bool need_remove;
    int counter_references;
    
    string UUID;
    uint64_t tms;
    
    UploadFile(string _UUID, uint64_t _tms)
    {
        UUID          = _UUID;
        tms           = _tms;
        time_created  = time(NULL);
        file          = NULL;
        fullFilePath  = "";
        status        = STATUS_DOWNLOAD_FILE__WAIT_START;
        mtu           = NULL;
        need_remove          = false;
        counter_references   = 0;
        
        pthread_rwlock_init(&lock_item, NULL);
    }
    
    ~UploadFile()
    {
        pthread_rwlock_destroy(&lock_item);
        
        if( file != NULL )
        {
            fclose(file);
        }
        
        if( mtu != NULL )
        {
            delete mtu;
            mtu = NULL;
        }
        
        printf("~UploadFile \n");
    }
    
    void lockREAD(int line, const char* function_name)
    {
        __LOCK_READ(&lock_item, line, function_name);
    }
    
    void lockWRITE(int line, const char* function_name)
    {
        __LOCK_WRITE(&lock_item, line, function_name);
    }
    
    void lockFREE(int line, const char* function_name)
    {
        __LOCK_FREE(&lock_item, line, function_name);
    }
};
//-----------------------------------------------------------------------------------------
struct SendData
{
    // ++ UDP info ---------------
    int soket, random_value;
    struct sockaddr_in si_other;
    socklen_t slen;
    string address_and_port;
    // -- UDP info ---------------
    
    uint64_t timestamp;
    
    string cmd;
    vector<string> params;
    struct buf* add_output_byffer;
    
    int count_send;                 // попытки отправки данных - счётчик
    uint64_t interval_send;         // интервал отправки
    uint64_t timestamp_last_send;
    bool need_remove;               // метка удаления

    //bool OK_SEND;
    uint8_t type_send;

    pthread_rwlock_t lock_item;
    
    uint8_t callback_typedata;
    void* callback_data;
    void (*callback_okSend)(SendData *sd, uint8_t typedata, void *data);
    void (*callback_errorSend)(SendData *sd, uint8_t typedata, void *data);

    SendData()
    {
        random_value         = 0;
        cmd                  = "";
        params               = vector<string>();
        add_output_byffer    = NULL;
        //OK_SEND              = false;
        type_send            = TYPE_SEND_DEFAULT;
        
        callback_okSend      = NULL;
        callback_errorSend   = NULL;
        callback_data        = NULL;

        soket                = 0;
        slen                 = 0;
        count_send           = 0;
        timestamp_last_send  = 0;
        need_remove          = false;
        interval_send        = INTERVAL_SEND_DATA_MS;
        timestamp            = 0;

        pthread_rwlock_init(&lock_item, NULL);
    }

    ~SendData()
    {
        pthread_rwlock_destroy(&lock_item);

        if( add_output_byffer != NULL )
        {
            buf_free( add_output_byffer );
            add_output_byffer = NULL;
        }
        
        if( callback_data != NULL)
        {
            if(callback_typedata == SEND_DATA_TYPE_CALBACK_DATA___CHECK_MTU)
            {
                delete ((CheckMTU_STEP *) callback_data);
            }
            else if(callback_typedata == SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE)
            {
                UploadFile* df = (UploadFile *) callback_data;
                
                if(df != NULL)
                {
                    df->lockWRITE(__LINE__, __FUNCTION__ );
                
                    df->counter_references -= 1;
                    
                    df->lockFREE(__LINE__, __FUNCTION__ );
                }
            }
        }
    }
    
    void copyNetInfo(SendData *sd)
    {
        memcpy(&si_other, &sd->si_other, sizeof(struct sockaddr_in));
        soket            = sd->soket;
        slen             = sd->slen;
        address_and_port = sd->address_and_port;
        random_value     = sd->random_value;
    }
    
    void copyNetInfo(ConnectionInfo *ci)
    {
        memcpy(&si_other, &ci->remote_addr, sizeof(struct sockaddr_in));
        soket            = ci->sockfd;
        address_and_port = ci->address_and_port;
        random_value     = ci->rand_value;
    }
    
    void lockREAD(int line, const char* function_name)
    {
        __LOCK_READ(&lock_item, line, function_name);
    }
    
    void lockWRITE(int line, const char* function_name)
    {
        __LOCK_WRITE(&lock_item, line, function_name);
    }
    
    void lockFREE(int line, const char* function_name)
    {
        __LOCK_FREE(&lock_item, line, function_name);
    }
};

std::atomic<int_fast16_t> totalUseMemoryQueueSend(0);
vector<SendData*> queue_send;
pthread_rwlock_t rwlock__queue_send;
bool wait_queue_send = false;

map<string, uint64_t> WAIT_SEND_SOCKET;
pthread_rwlock_t rwlock_wait_send_socket;

const int sizestructSendData = sizeof(struct SendData);
///-----------------------------------------------------------------------------
struct InputCmdReg
{
    string address_and_port;
    uint64_t timestamp;
};

vector<InputCmdReg*> register_input;
pthread_rwlock_t rwlock__register_input;
///-----------------------------------------------------------------------------
ssize_t _send_data_from_socket_crypt_encode(
                    int soket
                  , struct sockaddr_in si_other
                  , short random_value
                  , int *sended_length
                  , uint64_t timestamp
                  , const char* send_cmd
                  , uint8_t xor_key[16]
                  , vector<string> params = vector<string>()
                  , struct buf* add_output_byffer = NULL
            );

void _okSend(SendData *sd, uint8_t typedata, void *data);
void _errorSend(SendData *sd, uint8_t typedata, void *data);
uint64_t get_new_timestamp(bool need_block);

void __sendData(ConnectionInfo* ci, string cmd, vector<string> params = vector<string>(), struct buf* add_output_byffer = NULL);
///-----------------------------------------------------------------------------
///------------------------------------------------------------------------------------------
uint8_t circular_left_shift(uint8_t input, int n)
{
    uint8_t size = 8;
    uint8_t result;
     
    n = n % size;
    result = input << n | input >> (size - n);
    return result;
}
///------------------------------------------------------------------------------------------
uint8_t circular_right_shift(uint8_t input, int n)
{
    uint8_t size = 8;
    uint8_t result;
 
    n = n % size;
    result = input >> n | input << (size - n);
    return result;
}
///------------------------------------------------------------------------------------------
uint8_t *get_key(float r1, float r2, int len)
{
    uint8_t ff[] = { 0x58, 0xd6, 0xa2, 0x45, 0x3b, 0xa0 };
    
    uint8_t *result = (uint8_t*) malloc( len * 8 );
    int offset = 0;
    float i= 0, step = 360.0f / (float) len;
    
    for(int f = 0; f < len; f++)
    {
        float x = r1 * cos( i * M_PI / 180.0 );
        float y = r2 * sin( i * M_PI / 180.0 );
        
        x = round(x * 1000);
        y = round(y * 1000);
        
        memcpy(result + offset, &x, 4); offset += 4;
        memcpy(result + offset, &y, 4); offset += 4;
        
        i += step;
    }
    
    for(int f = 0; f < len * 8; f++)
    {
        result[f] ^= ff[ f % 6 ];
        result[f] = circular_left_shift(result[f], (ff[ f % 6 ] + 1) % 8);
        result[f] = ~result[f];
    }
    
    return result;
}
///------------------------------------------------------------------------------------------
uint8_t* _af5789(uint8_t* data, uint32_t len, uint8_t *key, uint8_t key_len, int encrypt)
{
    uint8_t* s = (uint8_t*)calloc( len, 1 );
    uint32_t i = 0;
    uint8_t shift, shift2, tmp;

    memcpy(s, data, len);

    size_t f = 0;
    for(i = 0; i < len; i++)
    {
        shift  = i*3 % 8;
        shift2 = i*2 % 8;
        if(shift == 0)
        {
            shift = 3;
        }
        
        if( encrypt == 1 )
        {
            s[i] ^= key[f++ % key_len];
            s[i] = circular_left_shift(s[i], shift);
            s[i] = ~s[i];
            s[i] ^= iv[i % 16];
            s[i] = circular_left_shift(s[i], shift2);
            s[i] ^= iv2[i % 16];
        }
        else
        {
            s[i] ^= iv2[i % 16];
            s[i] = circular_right_shift(s[i], shift2);
            s[i] ^= iv[i % 16];
            s[i] = ~s[i];
            s[i] = circular_right_shift(s[i], shift);
            s[i] ^= key[f++ % key_len];
        }
    }
    
    return s;
}
///------------------------------------------------------------------------------------------
uint8_t *_encrypt(uint8_t *input, unsigned int len_input, float f1, float f2, int f3)
{
    uint8_t *key    = get_key(f1, f2, f3);
    uint8_t *result = _af5789(input, len_input, key, f3 * 8, 1);
    
    free(key);
    return result;
}

uint8_t *_decrypt(uint8_t *input, unsigned int len_input, float f1, float f2, int f3)
{
    uint8_t *key    = get_key(f1, f2, f3);
    uint8_t *result = _af5789(input, len_input, key, f3 * 8, 2);
    
    free(key);
    return result;
}
///------------------------------------------------------------------------------------------
unsigned char* simple_xor(unsigned char* data, uint32_t len, uint8_t *key, uint8_t key_len)
{
    unsigned char* s = (unsigned char*)calloc( len, 1 );
    uint32_t i = 0;

    memcpy(s, data, len);

    size_t f = 0;
    for(i = 0; i < len; i++)
    {
        s[i] ^= key[f++ % key_len];
    }
    return s;
}
///------------------------------------------------------------------------------------------
void __inc_totalUseMemoryQueueSend(SendData *data)
{
    totalUseMemoryQueueSend += sizestructSendData + ( data->add_output_byffer != NULL ? data->add_output_byffer->len : 0 );
}
///------------------------------------------------------------------------------------------
void __dec_totalUseMemoryQueueSend(SendData *data)
{
    totalUseMemoryQueueSend -= sizestructSendData + ( data->add_output_byffer != NULL ? data->add_output_byffer->len : 0 );
}
///-----------------------------------------------------------------------------
void msleep(unsigned int msec)
{
    struct timespec timeout0;
    struct timespec timeout1;
    struct timespec* tmp;
    struct timespec* t0 = &timeout0;
    struct timespec* t1 = &timeout1;

    t0->tv_sec = msec / 1000;
    t0->tv_nsec = (msec % 1000) * (1000 * 1000);

    while(nanosleep(t0, t1) == -1)
    {
        if(errno == EINTR)
        {
            tmp = t0;
            t0 = t1;
            t1 = tmp;
        }
        else
            return;
    }
    return;
}
///-----------------------------------------------------------------------------
ssize_t _send_data_from_socket_crypt_encode(
                int soket
              , struct sockaddr_in si_other
              , short random_value
              , int *sended_length
              , uint64_t timestamp
              , const char* send_cmd
              , uint8_t xor_key[16]
              , vector<string> params
              , struct buf* add_output_byffer
        )
{
    unsigned char *res_encode = NULL, *res_encode_0 = NULL;
    struct buf* output_byffer = (struct buf*)buf_new();
    struct buf* encode_byffer = (struct buf*)buf_new();
    
    int total_len = 0;
    int len_cmd = strlen(send_cmd), params_counts = params.size();
    uint32_t _len_add_byffer = 0;

    total_len += 2;                           // *-
    total_len += 8;                           // timestamp
    total_len += 4 + strlen(send_cmd);        // название команды
    total_len += 1;                           // всего параметров
    
    
    for(int i = 0; i < params.size(); i++)
    {
        total_len += 2 + params[i].length();
    }
    
    total_len += 4; // длинна буффера binare
    
    if( add_output_byffer != NULL )
    {
        total_len += add_output_byffer->len;
    }
    
    initrand();
    
    // here encrypt parameters
    float f1 = randfloat(10, 80);
    float f2 = randfloat(10, 80);
    short f3 = 4 + myrandom(4);
    
    printf("fff: %.3f %.3f %d\n", f1, f2, f3);
    
    printf("send: random_value = %d len_cmd %d timestamp = %ld\n", random_value, len_cmd, timestamp);
    
    buf_append_u8( encode_byffer, '*' );
    buf_append_u8( encode_byffer, '-' );
    buf_append_data(encode_byffer, &timestamp, sizeof(uint64_t) );
    buf_append_data(encode_byffer, &len_cmd, 4 );
    buf_append_data(encode_byffer, (void*) send_cmd, len_cmd );
    
    /// всего параметров
    buf_append_u8(encode_byffer,  (uint8_t) params_counts );
    
    for(unsigned int i = 0; i < params_counts; i++)
    {
        uint16_t _len_us = (uint16_t) params[i].length();
        _len_us = htons( (uint16_t) _len_us );

        //printf("send parameter [%d] = %d %s\n", i, params[i].length(), params[i].c_str() );

        buf_append_u16(encode_byffer,  _len_us );
        buf_append_data(encode_byffer, (void*)params[i].c_str(), params[i].length() );
    }
    
    if( add_output_byffer != NULL )
    {
        _len_add_byffer = add_output_byffer->len;
    }

    buf_append_u32(encode_byffer,  htonl( (uint32_t) _len_add_byffer ) );

    if( add_output_byffer != NULL )
    {
        //printf("add_output_byffer->len: %d\n", add_output_byffer->len);

        buf_append_data(encode_byffer, add_output_byffer->ptr, add_output_byffer->len);
    }
    
    res_encode_0 = _encrypt(encode_byffer->ptr, encode_byffer->len, f1, f2, f3 );
    res_encode   = simple_xor(res_encode_0, encode_byffer->len, xor_key, 16 );
    
    //crc32 calc
    uint32_t crc_buffer = xcrc32( (uint8_t *)res_encode, encode_byffer->len, 0xffffffff );
    
    buf_append_data(output_byffer, &random_value, 2); 
    buf_append_u32(output_byffer,  htonl( (uint32_t) encode_byffer->len ) );
    buf_append_data(output_byffer, &f1, 4);
    buf_append_data(output_byffer, &f2, 4);
    buf_append_data(output_byffer, &f3, 2);
    
    
    textcolor(BRIGHT, YELLOW, BLACK);
    printf( "> %s\n", send_cmd );
    textcolor(RESET, WHITE, BLACK);
    //printf("send: random_value = %d %d  %3.4f %3.4f  %d\n", random_value, encode_byffer->len, f1, f2, f3);
    
    buf_append_data(output_byffer, res_encode, encode_byffer->len);
    buf_append_u32(output_byffer, htonl( crc_buffer ));
    
    // возвращает число переданных байт в случае успешного завершения и -1 в противном случае.
    int result = sendto(  
              soket
            , output_byffer->ptr
            , output_byffer->len
            , 0
            , (struct sockaddr*) &si_other
            , sizeof(si_other)
        );
    
    //********************
    
    *sended_length = output_byffer->len;

    buf_free(output_byffer);
    buf_free(encode_byffer);
    
    free(res_encode);
    free(res_encode_0);

    return result;
}
///-----------------------------------------------------------------------------
void *thread_add_SendData(void *arg)
{
    SendData *_tmp = (SendData *) arg;
    
    __LOCK_WRITE(&rwlock__queue_send, __LINE__, __FUNCTION__);

    __inc_totalUseMemoryQueueSend(_tmp);

    _tmp->timestamp = get_new_timestamp(false);
    queue_send.push_back( _tmp );

    __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );
    
    pthread_exit(0);
}
//------------------------------------------------------------------------------
void _okSend(SendData *sd, uint8_t typedata, void *data)
{
    printf("_okSend %d\n", typedata);
    
    if( typedata == SEND_DATA_TYPE_CALBACK_DATA___CHECK_MTU )
    {
        CheckMTU_STEP *step = (CheckMTU_STEP *) data;
        
        step->lask_k = step->test;
        step->counter_test++;
        
        if( step->count_iteration == 1 )
        {
            printf(".[%s] lask_k=%d\n", sd->address_and_port.c_str(), step->lask_k);
            return;
        }
        else
        {
            step->test += step->diff;
        }
        
        step->count_iteration++;
        //printf("+[%s] mtu=%d\n", sd->address_and_port.c_str(), step->test);
        
        ///------------------------------------------------------------------
        SendData* _tmp = new SendData();

        _tmp->copyNetInfo(sd);
        
        _tmp->cmd                 = "CHECK_MTU";

        _tmp->add_output_byffer   = (struct buf*)buf_new();

        _tmp->interval_send       = 250;
        _tmp->callback_data       = (void *) ( new CheckMTU_STEP( step ) );
        _tmp->callback_typedata   = SEND_DATA_TYPE_CALBACK_DATA___CHECK_MTU;
        _tmp->callback_okSend     = _okSend;
        _tmp->callback_errorSend  = _errorSend;

        uint8_t *test_buf = (uint8_t *) malloc( step->test );
                
        buf_append_data(_tmp->add_output_byffer, test_buf, step->test);

        free(test_buf);
        
        ///------------------------------------------------------------------
        pthread_attr_t pattr_thread_add_SendData;
        pthread_t child_thread_add_SendData;
        // set thread create attributes
        pthread_attr_init(&pattr_thread_add_SendData);
        pthread_attr_setdetachstate(&pattr_thread_add_SendData, PTHREAD_CREATE_DETACHED);

        pthread_create(&child_thread_add_SendData, &pattr_thread_add_SendData, thread_add_SendData, _tmp);
        ///-----------------------------------------------------------------
    }
    else if( typedata == SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE )
    {
        //__LOCK_READ( &rwlock_download_tasks, __LINE__, __FUNCTION__ );
        
            UploadFile *df = (UploadFile *) data;
            
            if( df == NULL )
            {
                printf("df == NULL %d\n", __LINE__);
                return;
            }

            df->lockWRITE(__LINE__, __FUNCTION__ );

            if( ! df->need_remove )
            {
                if( df->status == STATUS_DOWNLOAD_FILE__CHECK_MTU )
                {
                    CheckMTU_STEP *step = (CheckMTU_STEP *) df->mtu;

                    step->lask_k = step->test;
                    step->counter_test++;
                    
                    printf("ok send mtu lask_k = %d\n", step->lask_k);

                    if( step->count_iteration == 1 )
                    {
                        printf(".[%s] %d lask_k=%d\n", sd->address_and_port.c_str(), step->count_iteration, step->lask_k);
                        df->status = STATUS_DOWNLOAD_FILE__RUN_UPLOAD;
                        df->lockFREE(__LINE__, __FUNCTION__ );
                        return;
                    }
                    else
                    {
                        step->test += step->diff;
                    }

                    step->count_iteration++;
                    //printf("+[%s] mtu=%d\n", sd->address_and_port.c_str(), step->test);

                    ///------------------------------------------------------------------
                    SendData* _tmp = new SendData();

                    for(int f = 0; f < step->params.size(); f++)
                    {
                        _tmp->params.push_back( step->params[f] );
                    }

                    _tmp->copyNetInfo(sd);

                    _tmp->cmd                 = "CHECK_MTU";

                    _tmp->add_output_byffer   = (struct buf*)buf_new();

                    _tmp->interval_send       = 250;
                    _tmp->callback_data       = (void *) ( df );
                    _tmp->callback_typedata   = SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE;
                    _tmp->callback_okSend     = _okSend;
                    _tmp->callback_errorSend  = _errorSend;

                    uint8_t *test_buf = (uint8_t *) malloc( step->test );

                    buf_append_data(_tmp->add_output_byffer, test_buf, step->test);

                    free(test_buf);
                    
                    df->counter_references += 1;

                    ///------------------------------------------------------------------
                    pthread_attr_t pattr_thread_add_SendData;
                    pthread_t child_thread_add_SendData;
                    // set thread create attributes
                    pthread_attr_init(&pattr_thread_add_SendData);
                    pthread_attr_setdetachstate(&pattr_thread_add_SendData, PTHREAD_CREATE_DETACHED);

                    pthread_create(&child_thread_add_SendData, &pattr_thread_add_SendData, thread_add_SendData, _tmp);
                    ///-----------------------------------------------------------------
                }
            }

            df->lockFREE(__LINE__, __FUNCTION__ );
            
        //__LOCK_FREE( &rwlock_download_tasks, __LINE__, __FUNCTION__ );
    }
}
//------------------------------------------------------------------------------
void _errorSend(SendData *sd, uint8_t typedata, void *data)
{
    printf("_errorSend %d\n", typedata);
    
    if( typedata == SEND_DATA_TYPE_CALBACK_DATA___CHECK_MTU )
    {
        CheckMTU_STEP *step = (CheckMTU_STEP *) data;
        
        if( step->count_iteration > 15 )
        {
            printf("..[%s] lask_k=%d\n", sd->address_and_port.c_str(), step->lask_k);
            return;
        }
        
        uint32_t k_old = step->test;
        
        if( step->counter_test > 0 )
        {
            step->test -= ( step->test - step->lask_k ) / 2;
        }
        else
        {
            step->test = step->test / 2;
        }
        
        step->diff = ( k_old - step->test ) / 2;
        
        if( step->diff == 0 )
        {
            printf(".[%s] lask_k=%d\n", sd->address_and_port.c_str(), step->lask_k);
            return;
        }
        
        step->count_iteration++;
        printf("+[%s] test mtu=%d\n", sd->address_and_port.c_str(), step->test);
        
        ///------------------------------------------------------------------
        SendData* _tmp = new SendData();

        _tmp->copyNetInfo(sd);
        
        _tmp->cmd                 = "CHECK_MTU";

        _tmp->add_output_byffer   = (struct buf*)buf_new();

        _tmp->interval_send       = 250;
        _tmp->callback_data       = (void *) ( new CheckMTU_STEP( step ) );
        _tmp->callback_typedata   = SEND_DATA_TYPE_CALBACK_DATA___CHECK_MTU;
        _tmp->callback_okSend     = _okSend;
        _tmp->callback_errorSend  = _errorSend;

        //buf_extend(_tmp->add_output_byffer, step->test);
        uint8_t *test_buf = (uint8_t *) malloc( step->test );
                
        buf_append_data(_tmp->add_output_byffer, test_buf, step->test);

        free(test_buf);

        ///------------------------------------------------------------------
        pthread_attr_t pattr_thread_add_SendData;
        pthread_t child_thread_add_SendData;
        // set thread create attributes
        pthread_attr_init(&pattr_thread_add_SendData);
        pthread_attr_setdetachstate(&pattr_thread_add_SendData, PTHREAD_CREATE_DETACHED);

        pthread_create(&child_thread_add_SendData, &pattr_thread_add_SendData, thread_add_SendData, _tmp);
        ///-----------------------------------------------------------------
    }
    else if( typedata == SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE )
    {
        //__LOCK_READ( &rwlock_download_tasks, __LINE__, __FUNCTION__ );
        
            UploadFile *df = (UploadFile *) data;

            if( df == NULL )
            {
                printf("df == NULL %d\n", __LINE__);
                return;
            }

            df->lockWRITE(__LINE__, __FUNCTION__ );
        
            if( ! df->need_remove )
            {
                if( df->status == STATUS_DOWNLOAD_FILE__CHECK_MTU )
                {
                    CheckMTU_STEP *step = (CheckMTU_STEP *) df->mtu;

                    /*if( step->count_iteration > 20 )
                    {
                        printf("..[%s] lask_k=%d\n", sd->address_and_port.c_str(), step->lask_k);
                        df->status = STATUS_DOWNLOAD_FILE__RUN_UPLOAD;
                        df->lockFREE(__LINE__, __FUNCTION__ );
                        return;
                    }*/

                    uint32_t k_old = step->test;

                    if( step->counter_test > 0 )
                    {
                        step->test -= ( step->test - step->lask_k ) / 2;
                    }
                    else
                    {
                        step->test = step->test / 2;
                    }

                    step->diff = ( k_old - step->test ) / 2;

                    if( step->diff == 0 )
                    {
                        printf(".[%s] %d lask_k=%d\n", sd->address_and_port.c_str(), step->count_iteration, step->lask_k);

                        if(step->count_iteration == 0)
                        {
                            df->status = STATUS_DOWNLOAD_FILE__ERROR_NEED_REMOVE_TASK;
                        }
                        else
                        {
                            df->status = STATUS_DOWNLOAD_FILE__RUN_UPLOAD;
                        }

                        df->lockFREE(__LINE__, __FUNCTION__ );
                        return;
                    }

                    step->count_iteration++;
                    //printf("+[%s] mtu=%d\n", sd->address_and_port.c_str(), step->test);

                    ///------------------------------------------------------------------
                    SendData* _tmp = new SendData();

                    for(int f = 0; f < step->params.size(); f++)
                    {
                        _tmp->params.push_back( step->params[f] );
                    }

                    _tmp->copyNetInfo(sd);

                    _tmp->cmd                 = "CHECK_MTU";

                    _tmp->add_output_byffer   = (struct buf*)buf_new();

                    _tmp->interval_send       = 250;
                    _tmp->callback_data       = (void *) ( df );
                    _tmp->callback_typedata   = SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE;
                    _tmp->callback_okSend     = _okSend;
                    _tmp->callback_errorSend  = _errorSend;

                    uint8_t *test_buf = (uint8_t *) malloc( step->test );

                    buf_append_data(_tmp->add_output_byffer, test_buf, step->test);

                    free(test_buf);
                    
                    df->counter_references += 1;

                    ///------------------------------------------------------------------
                    pthread_attr_t pattr_thread_add_SendData;
                    pthread_t child_thread_add_SendData;
                    // set thread create attributes
                    pthread_attr_init(&pattr_thread_add_SendData);
                    pthread_attr_setdetachstate(&pattr_thread_add_SendData, PTHREAD_CREATE_DETACHED);

                    pthread_create(&child_thread_add_SendData, &pattr_thread_add_SendData, thread_add_SendData, _tmp);
                    ///-----------------------------------------------------------------
                }
                else if( df->status == STATUS_DOWNLOAD_FILE__RUN_UPLOAD )
                {
                    df->need_remove = true;
                }
            }

            df->lockFREE(__LINE__, __FUNCTION__ );
        
        //__LOCK_FREE( &rwlock_download_tasks, __LINE__, __FUNCTION__ );
    }
}
///------------------------------------------------------------------------------------------
void *threadQueueOutput(void *arg)
{
    pthread_setname_np(pthread_self(), "thread_queue_output");
    
    ssize_t status_send;
    bool next_iteration = true;
    int count_remove    = 0;

    while(true)
    {
        if( wait_queue_send )
        {
            msleep(5);
        }
        
        __LOCK_READ( &rwlock__queue_send, __LINE__, __FUNCTION__ );
        
        for(unsigned int i = 0, __len = queue_send.size(); i < __len; i++)
        {
            if( wait_queue_send )
            {
                break;
            }
            
            SendData* _item = queue_send[ i ];
            
            _item->lockWRITE(__LINE__, __FUNCTION__);

            if(
                    _item->need_remove == false
                &&  get_time_in_ms() - _item->timestamp_last_send > _item->interval_send )
            {
                if(  _item->type_send == TYPE_SEND_DEFAULT )
                {
                    __LOCK_WRITE( &rwlock_wait_send_socket, __LINE__, __FUNCTION__ );

                    map<string, uint64_t>::iterator __it = WAIT_SEND_SOCKET.find( _item->address_and_port );

                    if(  __it != WAIT_SEND_SOCKET.end() && __it->second != _item->timestamp )
                    {
                        //printf("- skip item: [%ld] %s %s\n", _item->timestamp, _item->address_and_port.c_str(), _item->cmd.c_str());

                        __LOCK_FREE( &rwlock_wait_send_socket, __LINE__, __FUNCTION__ );
                        _item->lockFREE(__LINE__, __FUNCTION__);
                        continue;
                    }

                    __LOCK_FREE( &rwlock_wait_send_socket, __LINE__, __FUNCTION__ );
                }
                
                _item->timestamp_last_send = get_time_in_ms();

                //if( _item->OK_SEND == false )  /// __НЕ__было__подтврждения__OK_SEND
                {
                    int sended_length = 0;
                    
                    status_send = _send_data_from_socket_crypt_encode(
                                                  _item->soket
                                                , _item->si_other
                                                , _item->random_value
                            
                                                , &sended_length
                                                , _item->timestamp
                                                , _item->cmd.c_str()
                                                , default_xor_key
                                                , _item->params
                                                , _item->add_output_byffer
                                               );

                    if( status_send == -1 )
                    {
                        _item->need_remove = true;
                    
                        if( _item->callback_errorSend != NULL )
                        {
                            _item->callback_errorSend(_item, _item->callback_typedata, _item->callback_data);
                        }
                    }
                    else
                    {
                        _item->count_send += 1;
                    }
                }

                if( /*_item->OK_SEND == true ||*/ _item->count_send >= LIMIT_COUNT_SEND )
                {
                    _item->need_remove = true;
                    
                    if( _item->callback_errorSend != NULL )
                    {
                        _item->callback_errorSend(_item, _item->callback_typedata, _item->callback_data);
                    }
                }
            }

            _item->lockFREE(__LINE__, __FUNCTION__ );
        }

        __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );

        msleep(2);

        /// удаление
        __LOCK_WRITE( &rwlock__queue_send, __LINE__, __FUNCTION__ );

        next_iteration = true;
        count_remove = 0;

        while(next_iteration)
        {
            next_iteration = false;

            for(unsigned int i = 0, len = queue_send.size(); i < len; i++)
            {
                SendData* _item = queue_send[ i ];

                _item->lockWRITE(__LINE__, __FUNCTION__ );

                if( _item->need_remove == true )
                {
                    if( _item->type_send == TYPE_SEND_DEFAULT )
                    {
                        __LOCK_WRITE( &rwlock_wait_send_socket, __LINE__, __FUNCTION__ );

                        map<string, uint64_t>::iterator __it = WAIT_SEND_SOCKET.find( _item->address_and_port );

                        if( __it != WAIT_SEND_SOCKET.end() )
                        {
                            printf("WAIT_SEND_SOCKET - delete2: [%ld] %s %s\n", _item->timestamp, _item->address_and_port.c_str(), _item->cmd.c_str());
                            WAIT_SEND_SOCKET.erase( __it );
                        }

                        __LOCK_FREE( &rwlock_wait_send_socket, __LINE__, __FUNCTION__ );
                    }
                    
                    __dec_totalUseMemoryQueueSend(_item);
                    
                    delete _item;

                    queue_send.erase( queue_send.begin() + i );

                    next_iteration = true;
                    break;
                }

                _item->lockFREE(__LINE__, __FUNCTION__ );
            }
            
            count_remove += 1;
            
            if(count_remove > 10)
            {
                break;
            }
        }

        __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );

        msleep(2);

        /// очистка
        __LOCK_WRITE( &rwlock__register_input, __LINE__, __FUNCTION__ ); /// запись

        if( register_input.size() > LIMIT_SIZE_INPUT_REGISTER )
        {
            delete register_input[0];
            register_input[ 0 ] = NULL;
            
            register_input.erase( register_input.begin() ); /// удалить с начала
        }

        __LOCK_FREE( &rwlock__register_input, __LINE__, __FUNCTION__ );
        
        msleep(1);
    
    } // --while

    pthread_exit(0);          /* terminate the thread */
}
///-----------------------------------------------------------------------------
ConnectionInfo *initUdpSocket(int server_port)
{
    ConnectionInfo *connectionInfo = new ConnectionInfo();
    
    // Creating socket file descriptor 
    if ( (connectionInfo->sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0 ) { 
        perror("socket creation failed");
        
        delete connectionInfo;
        exit(EXIT_FAILURE); 
    }
    
    bzero((char *) &connectionInfo->remote_addr, sizeof(connectionInfo->remote_addr));
    
    // Filling server information 
    connectionInfo->remote_addr.sin_family      = AF_INET; 
    connectionInfo->remote_addr.sin_port        = htons(server_port); 
    connectionInfo->remote_addr.sin_addr.s_addr = inet_addr(SERVER_HOST);
    memset(connectionInfo->remote_addr.sin_zero, '\0', sizeof(connectionInfo->remote_addr.sin_zero));
    
    
    int remote_connect = connect(connectionInfo->sockfd, (struct sockaddr *)&connectionInfo->remote_addr, sizeof(struct sockaddr));
    if(remote_connect == -1)
    {
      perror("Connect ");
      exit(1);
    }
    
    connectionInfo->address_and_port = string_format(
                        "%d:%s:%d"
                      , connectionInfo->rand_value
                      , inet_ntoa(connectionInfo->remote_addr.sin_addr)
                      , ntohs(connectionInfo->remote_addr.sin_port)
                );
    
    printf("connectionInfo->address_and_port: %s\n", connectionInfo->address_and_port.c_str());
    
    return connectionInfo;
}
///-----------------------------------------------------------------------------
bool __exec_h(ReadCommand *data)
{
    string cmd = data->cmd;

    if(strcasecmp(cmd.c_str(), "OK_SEND") == 0) /// принят запрос подтверждения
    {
        __LOCK_READ( &rwlock__queue_send, __LINE__, __FUNCTION__ );

        for(unsigned int i = 0, len = queue_send.size(); i < len; i++)
        {
            SendData* _item = queue_send[ i ];
            
            _item->lockWRITE(__LINE__, __FUNCTION__); /// запись

            if(
                      _item->need_remove == false
                   && _item->timestamp   == data->timestamp
                   && _item->address_and_port.compare(data->ci->address_and_port) == 0
              )
            {
                //_item->OK_SEND = true;
                _item->need_remove = true;
                
                if( _item->callback_okSend != NULL )
                {
                    _item->callback_okSend(_item, _item->callback_typedata, _item->callback_data);
                }
                
                
                /*__LOCK_WRITE( &rwlock_wait_send_socket, __LINE__, __FUNCTION__ );
                
                map<string, uint64_t>::iterator __it = WAIT_SEND_SOCKET.find( _item->address_and_port );
            
                if( __it != WAIT_SEND_SOCKET.end() )
                {
                    //printf("WAIT_SEND_SOCKET - delete: [%ld] %s\n", _item->timestamp, _item->address_and_port.c_str());
                    WAIT_SEND_SOCKET.erase( __it );
                }
                
                __LOCK_FREE( &rwlock_wait_send_socket, __LINE__, __FUNCTION__ );

                //__LOCK_FREE( &_item->lock_item );
                
                __dec_totalUseMemoryQueueSend(_item);
                
                delete _item;

                queue_send.erase( queue_send.begin() + i );
                break;*/
            }

            _item->lockFREE(__LINE__, __FUNCTION__);
        }

        __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );

        return true; // is OK_SEND
    }

    ///-------------------------------------------------------------------------------------------
    int sended_length = 0;
    /// отправить подтверждение
    _send_data_from_socket_crypt_encode(
              data->ci->sockfd
            , data->ci->remote_addr
            , data->ci->rand_value
            
            , &sended_length
            , data->timestamp
            , "OK_SEND"
            , default_xor_key);

    ///-------------------------------------------------------------------------------------------
    __LOCK_READ( &rwlock__register_input, __LINE__, __FUNCTION__ ); /// чтение

    bool founded_input = false;

    for(unsigned int i = 0, len = register_input.size(); i < len; i++ )
    {
        InputCmdReg *_tmp = register_input[i];

        if(
                _tmp->timestamp == data->timestamp
             && _tmp->address_and_port.compare(data->ci->address_and_port) == 0
          )
        {
            founded_input = true;
            break;
        }
    }

    __LOCK_FREE( &rwlock__register_input, __LINE__, __FUNCTION__ );

    /// ---- дублирование ----
    if( founded_input )
    {
        printf("diplicate %s %lu\n", data->ci->address_and_port.c_str(), data->timestamp);
        return true;
    }
    else
    {
        InputCmdReg *_tmp       = new InputCmdReg();
        _tmp->address_and_port  = data->ci->address_and_port;
        _tmp->timestamp         = data->timestamp;

        __LOCK_WRITE( &rwlock__register_input, __LINE__, __FUNCTION__ );

        register_input.push_back( _tmp );

        __LOCK_FREE( &rwlock__register_input, __LINE__, __FUNCTION__ );
    }

    return false;
}
///---------------------------------------------------------------------------------------------
uint64_t get_new_timestamp(bool need_block)
{
    if( need_block )
    {
	__LOCK_READ( &rwlock__queue_send, __LINE__, __FUNCTION__ );
    }

    uint64_t result = get_timestamp();

    bool flag = true;
    unsigned int i = 0, len = 0;

    while(flag)
    {
        flag = false;

        for(i = 0, len = queue_send.size(); i < len; i++)
        {
            SendData* _item = queue_send[ i ];

            _item->lockREAD(__LINE__, __FUNCTION__ ); /// чтение

            if( _item->timestamp == result )
            {
                result += 1;
                flag = true;
                _item->lockFREE(__LINE__, __FUNCTION__ );
                break;
            }

            _item->lockFREE(__LINE__, __FUNCTION__ );
        }
    }

    if( need_block )
    {
	__LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );
    }

    return result;
}
//------------------------------------------------------------------------------
void __sendData(ConnectionInfo* ci, string cmd, vector<string> params, struct buf* add_output_byffer)
{
    SendData *_tmp = new SendData();

    _tmp->copyNetInfo(ci);
    _tmp->cmd       = cmd;
    _tmp->params    = params;
    _tmp->add_output_byffer = add_output_byffer;
    
    __LOCK_WRITE(&rwlock__queue_send, __LINE__, __FUNCTION__);

    __inc_totalUseMemoryQueueSend(_tmp);

    _tmp->timestamp = get_new_timestamp(false);
    queue_send.push_back( _tmp );

    __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );
}
///-----------------------------------------------------------------------------
void _exec(ReadCommand *data)
{
    if(data == NULL)
    {
        printf("NULL data\n");
        return;
    }
        
#if DEBUG_PRINT
    //if( data->cmd.compare("OK_SEND") != 0 )
    {
        textcolor(BRIGHT, RED, BLACK);
        printf( "_exec: %s\n", data->cmd.c_str() );//, data->timestamp, data->udp_info->address_and_port.c_str() );
        textcolor(RESET, WHITE, BLACK);
    }
#endif
    
    if( __exec_h( data ) )
    {
        return;
    }
    
    char *cmd = (char*) data->cmd.c_str();
    
    if(    strcmp(cmd, "DOWNLOAD_BASE") == 0 
        && (data->params.size() == 1 || data->params.size() == 4) )
    {
        if( data->params.size() == 4 && strcmp(data->params[0].c_str(), "OK") == 0 )
        {
            string uuid         = data->params[1];
            string tms          = data->params[2];
            uint32_t file_size  = atol(data->params[3].c_str());
            
            printf("tms: %s  file_size: %lld\n", tms.c_str(), file_size);
            
            FILE *fp = fopen("test.dat", "wb");
            if (fp)
            {
                // Now go to the intended end of the file
                // (subtract 1 since we're writing a single character).
                fseek(fp, file_size - 1, SEEK_SET);
                // Write at least one byte to extend the file (if necessary).
                fwrite("", 1, sizeof(char), fp);
                fclose(fp);
                
                __sendData(data->ci, "DOWNLOAD_BASE_START", vector<string>{ uuid, tms }, NULL);
            }
            
        }
    }
    else if( strcmp(cmd, "DOWNLOAD_STEP") == 0 )
    {
        uint32_t pos;
        memcpy(&pos, data->buffer, 4);
        
        printf("pos: %ld %d\n", pos, data->len_buffer);
        
        FILE *fp = fopen("test.dat", "r+b");
        if (fp)
        {
            fseek(fp, pos, SEEK_SET);
            fwrite(data->buffer + 4, 1, data->len_buffer - 4, fp);
            fclose(fp);
        }
    }
    else if( strcmp(cmd, "DOWNLOAD_STEP_END") == 0 )
    {
        
    }
    
    
}
///-----------------------------------------------------------------------------
ReadCommand *_readBuffer(uint8_t *buf, int len)
{
    ReadCommand *res           = NULL;
    struct buf* encode_byffer  = NULL;
    int offset                 = 0;
    uint32_t _crc32            = 0;
    
    if( offset + 4 > len )
    {
        return res;
    }
    
    res = new ReadCommand();
    
    char check_ch1;
    memcpy(&check_ch1, buf + offset, 1); offset += 1;
    
    if( check_ch1 != '*' )
    {
        printf("error read *\n");
        return res;
    }
    
    uint32_t len_data = 0;
    
    memcpy(&len_data, buf + offset, 4); offset += 4;
    
    printf("len_data: %d %d\n", len_data, len );
    
    if( len_data < 0 || len_data + 1 + 4 + 4 + 4 + 2 + 1 + 4 != len )
    {
        printf("error: length not right\n");
        return res;
    }
    
    float f1, f2;
    short f3;
    
    memcpy(&f1, buf + offset, 4); offset += 4;
    memcpy(&f2, buf + offset, 4); offset += 4;
    memcpy(&f3, buf + offset, 2); offset += 2;
    
    printf("<-- fff: %.4f %.4f %d\n", f1, f2, f3);
    
    encode_byffer = (struct buf*) buf_new();
    buf_append_data(encode_byffer, buf + offset, len_data);  offset += len_data;
    
    memcpy(&check_ch1, buf + offset, 1); offset += 1;
    
    if( check_ch1 != '=' )
    {
        printf("error read =\n");
        return res;
    }
    
    
    memcpy(&_crc32, buf + offset, 4); //offset_1 += 4;

    printf("crc32: %ld\n", _crc32);
    
    uint32_t crc_buffer = xcrc32((unsigned char *)encode_byffer->ptr, encode_byffer->len, 0xffffffff);

    printf("crc32_2: %ld\n", crc_buffer);

    if( _crc32 != crc_buffer )
    {
        printf("error crc32_2\n");
        return res;
    }
    
    ///----------------------------------------------------------------------------------
    unsigned char *data_encoded_0 = simple_xor(encode_byffer->ptr, len_data, default_xor_key, 16);
    unsigned char *data_encoded   = _decrypt(data_encoded_0, len_data, f1, f2, f3);
    uint64_t timestamp;
    uint16_t len_cmd;
    uint8_t count_params;
    char *read_str = NULL;
    
    offset = 0;
    
    memcpy(&check_ch1, data_encoded + offset, 1); offset += 1;
    
    if( check_ch1 != '*' )
    {
        printf("error read_2 *\n");
        return res;
    }
    
    memcpy(&check_ch1, data_encoded + offset, 1); offset += 1;
    
    if( check_ch1 != '-' )
    {
        printf("error read_2 -\n");
        return res;
    }
    
    memcpy(&timestamp, data_encoded + offset, 8); offset += 8;
    memcpy(&len_cmd, data_encoded + offset, 2);   offset += 2;

    printf("timestamp: %ld  len_cmd: %ld\n", timestamp, len_cmd);
    
    read_str = (char*)calloc( len_cmd + 1, 1 );

    if(read_str == NULL)
    {
        buf_free( encode_byffer );
        free( data_encoded );
        free( data_encoded_0 );
        return res;
    }
    
    memcpy(read_str, data_encoded + offset, len_cmd); offset += len_cmd;

    res->cmd.append( read_str );
    res->timestamp = timestamp;

    printf("read_str: %s\n", read_str);
    
    if(read_str != NULL)
    {
        free(read_str);
        read_str = NULL;
    }
    
    memcpy(&count_params, data_encoded + offset, 1); offset += 1;

    printf("count_params: %d\n", count_params);

    for(uint8_t k = 0; k < count_params; k++)
    {
        string _s_str = "";
        uint16_t len = 0;

        memcpy(&len, data_encoded + offset, 2); offset += 2;

        if(len < 0 || len > 65536)
        {
            //printf("error read parameters \n");
            buf_free( encode_byffer );
            free( data_encoded );
            free( data_encoded_0 );
            return res;
        }

        read_str = (char*)calloc( len + 1, sizeof(char) );

        if(read_str == NULL)
        {
            buf_free( encode_byffer );
            free( data_encoded );
            free( data_encoded_0 );
            return res;
        }

        memcpy(read_str, data_encoded + offset, len); offset += len;

        _s_str.append( read_str );

        if(read_str != NULL)
        {
            free(read_str);
            read_str = NULL;
        }

        printf("_s_str: %s [%d]\n", _s_str.c_str(), len);
        res->params.push_back( _s_str );
    }
    
    int len_buffer = 0;

    memcpy(&len_buffer, data_encoded + offset, 4); offset += 4;

    printf("len_buffer: %d\n", len_buffer);

    res->len_buffer = len_buffer;

    res->buffer = (unsigned char*) calloc( len_buffer, 1 );

    memcpy(res->buffer, data_encoded + offset, len_buffer);

    res->init_ok = true;
    
    //*****************************
    buf_free( encode_byffer );
    
    return res;
}
///-----------------------------------------------------------------------------
int main(int argc, char** argv) {

    //string uuid = genUuid();
    
    initrand();
    
    ConnectionInfo *connectionInfo = initUdpSocket(SERVER_PORT);
    
    ///------------------------------------------------------------------
    pthread_attr_t pattr_threadQueueOutput;
    pthread_t child_threadQueueOutput;
    pthread_attr_init(&pattr_threadQueueOutput);
    pthread_attr_setdetachstate(&pattr_threadQueueOutput, PTHREAD_CREATE_DETACHED);

    pthread_create(&child_threadQueueOutput, &pattr_threadQueueOutput, threadQueueOutput, connectionInfo);
    ///------------------------------------------------------------------
    
    pthread_rwlock_init(&rwlock__queue_send, NULL);
    pthread_rwlock_init(&rwlock_wait_send_socket, NULL);
    pthread_rwlock_init(&rwlock__register_input, NULL);
    
    uint8_t buf[UDP_BUFLEN]; 
    
    int recv_len;
    
    
    int sended_length = 0, status_send = 0;
    
    uint64_t timestamp = get_timestamp();
    /*vector<string> params;
    params.push_back("TEST_1");
    params.push_back("TEST_2");
    params.push_back("TEST_3");
    
    __sendData(connectionInfo, "TEST_SEND_DATA", params, NULL);
    
    params.clear();
    params.push_back( genUuid() );
    
    __sendData(connectionInfo, "DOWNLOAD_BASE", params, NULL);*/
    
    __sendData(connectionInfo, "ECHO");
    
    //чтение данных с сервера
    struct sockaddr from;
    socklen_t slen;
    while( true )
    {
        slen = sizeof(connectionInfo->remote_addr); /* must be initialized */
        
        if( (recv_len = recvfrom(connectionInfo->sockfd, (char *)buf, UDP_BUFLEN, 0, (struct sockaddr *) &connectionInfo->remote_addr, &slen)) < 10)
        {
            printf("erro recvfrom()");
            continue;
        }

        printf("_readBuffer: %d\n", recv_len);

        ReadCommand *parse = _readBuffer(buf, recv_len);
        parse->ci = connectionInfo;
        
        if(parse->init_ok)
        {
            _exec(parse);
        }
        
        //break;
    }
  
    close(connectionInfo->sockfd);
    delete connectionInfo;
    return 0; 
}

