/*
 * The code is derived from cURL example and paster.c base code.
 * The cURL example is at URL:
 * https://curl.haxx.se/libcurl/c/getinmemory.html
 * Copyright (C) 1998 - 2018, Daniel Stenberg, <daniel@haxx.se>, et al..
 *
 * The xml example code is
 * http://www.xmlsoft.org/tutorial/ape.html
 *
 * The paster.c code is
 * Copyright 2013 Patrick Lam, <p23lam@uwaterloo.ca>.
 *
 * Modifications to the code are
 * Copyright 2018-2019, Yiqing Huang, <yqhuang@uwaterloo.ca>.
 *
 * This software may be freely redistributed under the terms of the X11 license.
 */

/**
 * @file main_wirte_read_cb.c
 * @brief cURL write call back to save received data in a user defined memory first
 *        and then write the data to a file for verification purpose.
 *        cURL header call back extracts data sequence number from header if there is a sequence number.
 * @see https://curl.haxx.se/libcurl/c/getinmemory.html
 * @see https://curl.haxx.se/libcurl/using/
 * @see https://ec.haxx.se/callback-write.html
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/uri.h>
#include <libxml/xmlstring.h>
#include <pthread.h>
#include <search.h>
#include <semaphore.h>

#define SEED_URL "http://ece252-1.uwaterloo.ca/lab4/"
#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_SIZE 1048576 /* 1024*1024 = 1M */
#define BUF_INC 524288   /* 1024*512  = 0.5M */

#define CT_PNG "image/png"
#define CT_HTML "text/html"
#define CT_PNG_LEN 9
#define CT_HTML_LEN 9

#define max(a, b) \
    ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

typedef struct recv_buf2
{
    char *buf;       /* memory to hold a copy of received data */
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    int seq;         /* >=0 sequence number extracted from http header */
                     /* <0 indicates an invalid seq number */
} RECV_BUF;

typedef struct node
{
    struct node *next;
    char *url;
} node;

typedef struct linked_list
{
    struct node *head;
} linked_list;

int log_file;
char v[256];
int img_count;
int m;
int t;
int barrier_count;
int first_loop;
int break_flag;
linked_list *url_frontier;
pthread_mutex_t count;
pthread_mutex_t frontier;
pthread_mutex_t barrier_lock;
pthread_cond_t cv;

htmlDocPtr mem_getdoc(char *buf, int size, const char *url);
xmlXPathObjectPtr getnodeset(xmlDocPtr doc, xmlChar *xpath);
int find_http(char *fname, int size, int follow_relative_links, const char *base_url);
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata);
size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata);
int recv_buf_init(RECV_BUF *ptr, size_t max_size);
int recv_buf_cleanup(RECV_BUF *ptr);
void cleanup(CURL *curl, RECV_BUF *ptr);
int write_file(const char *path, const void *in, size_t len);
CURL *easy_handle_init(RECV_BUF *ptr, const char *url);
int process_data(CURL *curl_handle, RECV_BUF *p_recv_buf, char *curr_url);

htmlDocPtr mem_getdoc(char *buf, int size, const char *url)
{
    int opts = HTML_PARSE_NOBLANKS | HTML_PARSE_NOERROR |
               HTML_PARSE_NOWARNING | HTML_PARSE_NONET;
    htmlDocPtr doc = htmlReadMemory(buf, size, url, NULL, opts);

    if (doc == NULL)
    {
        // fprintf(stderr, "Document not parsed successfully.\n");
        return NULL;
    }
    return doc;
}

xmlXPathObjectPtr getnodeset(xmlDocPtr doc, xmlChar *xpath)
{

    xmlXPathContextPtr context;
    xmlXPathObjectPtr result;

    context = xmlXPathNewContext(doc);
    if (context == NULL)
    {
        printf("Error in xmlXPathNewContext\n");
        return NULL;
    }
    result = xmlXPathEvalExpression(xpath, context);
    xmlXPathFreeContext(context);
    if (result == NULL)
    {
        printf("Error in xmlXPathEvalExpression\n");
        return NULL;
    }
    if (xmlXPathNodeSetIsEmpty(result->nodesetval))
    {
        xmlXPathFreeObject(result);
        // printf("No result\n");
        return NULL;
    }
    return result;
}

int hinsert(char *key)
{
    ENTRY entry;
    entry.key = key;
    if (hsearch(entry, FIND) != NULL)
    {
        // printf("entry already exists\n");
        return 0;
    }
    else
    {
        if (hsearch(entry, ENTER) == NULL)
        {
            printf("unable to add entry to table\n");
            return 0;
        }
        else
        {
            return 1;
        }
    }
    return 0;
}

int find_http(char *buf, int size, int follow_relative_links, const char *base_url)
{

    int i;
    htmlDocPtr doc;
    xmlChar *xpath = (xmlChar *)"//a/@href";
    xmlNodeSetPtr nodeset;
    xmlXPathObjectPtr result;
    xmlChar *href;

    if (buf == NULL)
    {
        return 1;
    }

    doc = mem_getdoc(buf, size, base_url);
    result = getnodeset(doc, xpath);
    if (result)
    {
        nodeset = result->nodesetval;
        for (i = 0; i < nodeset->nodeNr; i++)
        {
            href = xmlNodeListGetString(doc, nodeset->nodeTab[i]->xmlChildrenNode, 1);
            if (follow_relative_links)
            {
                xmlChar *old = href;
                href = xmlBuildURI(href, (xmlChar *)base_url);
                xmlFree(old);
            }
            if (href != NULL && !strncmp((const char *)href, "http", 4))
            {
                pthread_mutex_lock(&frontier);
                char *new_url = (char *)xmlStrdup(href);
                if (hinsert(new_url)) // if not an entry
                {
                    node *temp = malloc(sizeof(node));
                    temp->url = strdup((char *)href);
                    temp->next = url_frontier->head; // temp next is curr head
                    url_frontier->head = temp;       // head is new entry (temp)
                    if (log_file == 1)
                    {
                        FILE *fp = fopen(v, "a");
                        fwrite(temp->url, strlen(temp->url), 1, fp); // write png
                        fwrite("\n", sizeof(char), 1, fp);           // write new line
                        fclose(fp);
                    }
                }
                pthread_mutex_unlock(&frontier);
            }
            xmlFree(href);
        }
        xmlXPathFreeObject(result);
    }
    xmlFreeDoc(doc);
    return 0;
}
/**
 * @brief  cURL header call back function to extract image sequence number from
 *         http header data. An example header for image part n (assume n = 2) is:
 *         X-Ece252-Fragment: 2
 * @param  char *p_recv: header data delivered by cURL
 * @param  size_t size size of each memb
 * @param  size_t nmemb number of memb
 * @param  void *userdata user defined data structurea
 * @return size of header data received.
 * @details this routine will be invoked multiple times by the libcurl until the full
 * header data are received.  we are only interested in the ECE252_HEADER line
 * received so that we can extract the image sequence number from it. This
 * explains the if block in the code.
 */
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata)
{
    int realsize = size * nmemb;
    RECV_BUF *p = userdata;

#ifdef DEBUG1_
    printf("%s", p_recv);
#endif /* DEBUG1_ */
    if (realsize > strlen(ECE252_HEADER) &&
        strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0)
    {

        /* extract img sequence number */
        p->seq = atoi(p_recv + strlen(ECE252_HEADER));
    }
    return realsize;
}

/**
 * @brief write callback function to save a copy of received data in RAM.
 *        The received libcurl data are pointed by p_recv,
 *        which is provided by libcurl and is not user allocated memory.
 *        The user allocated memory is at p_userdata. One needs to
 *        cast it to the proper struct to make good use of it.
 *        This function maybe invoked more than once by one invokation of
 *        curl_easy_perform().
 */

size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata)
{
    size_t realsize = size * nmemb;
    RECV_BUF *p = (RECV_BUF *)p_userdata;

    if (p->size + realsize + 1 > p->max_size)
    { /* hope this rarely happens */
        /* received data is not 0 terminated, add one byte for terminating 0 */
        size_t new_size = p->max_size + max(BUF_INC, realsize + 1);
        char *q = realloc(p->buf, new_size);
        if (q == NULL)
        {
            perror("realloc"); /* out of memory */
            return -1;
        }
        p->buf = q;
        p->max_size = new_size;
    }

    memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
}

int recv_buf_init(RECV_BUF *ptr, size_t max_size)
{
    void *p = NULL;

    if (ptr == NULL)
    {
        return 1;
    }

    p = malloc(max_size);
    if (p == NULL)
    {
        return 2;
    }

    ptr->buf = p;
    ptr->size = 0;
    ptr->max_size = max_size;
    ptr->seq = -1; /* valid seq should be positive */
    return 0;
}

int recv_buf_cleanup(RECV_BUF *ptr)
{
    if (ptr == NULL)
    {
        return 1;
    }

    free(ptr->buf);
    ptr->size = 0;
    ptr->max_size = 0;
    return 0;
}

void cleanup(CURL *curl, RECV_BUF *ptr)
{
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    recv_buf_cleanup(ptr);
}

/**
 * @brief create a curl easy handle and set the options.
 * @param RECV_BUF *ptr points to user data needed by the curl write call back function
 * @param const char *url is the target url to fetch resoruce
 * @return a valid CURL * handle upon sucess; NULL otherwise
 * Note: the caller is responsbile for cleaning the returned curl handle
 */

CURL *easy_handle_init(RECV_BUF *ptr, const char *url)
{
    CURL *curl_handle = NULL;

    if (ptr == NULL || url == NULL)
    {
        return NULL;
    }

    /* init user defined call back function buffer */
    if (recv_buf_init(ptr, BUF_SIZE) != 0)
    {
        return NULL;
    }
    /* init a curl session */
    curl_handle = curl_easy_init();

    if (curl_handle == NULL)
    {
        fprintf(stderr, "curl_easy_init: returned NULL\n");
        return NULL;
    }

    /* specify URL to get */
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);

    /* register write call back function to process received data */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl3);
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)ptr);

    /* register header call back function to process received header data */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl);
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)ptr);

    /* some servers requires a user-agent field */
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "ece252 lab4 crawler");

    /* follow HTTP 3XX redirects */
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    /* continue to send authentication credentials when following locations */
    curl_easy_setopt(curl_handle, CURLOPT_UNRESTRICTED_AUTH, 1L);
    /* max numbre of redirects to follow sets to 5 */
    curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 5L);
    /* supports all built-in encodings */
    curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "");

    /* Max time in seconds that the connection phase to the server to take */
    // curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 5L);
    /* Max time in seconds that libcurl transfer operation is allowed to take */
    // curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);
    /* Time out for Expect: 100-continue response in milliseconds */
    // curl_easy_setopt(curl_handle, CURLOPT_EXPECT_100_TIMEOUT_MS, 0L);

    /* Enable the cookie engine without reading any initial cookies */
    curl_easy_setopt(curl_handle, CURLOPT_COOKIEFILE, "");
    /* allow whatever auth the proxy speaks */
    curl_easy_setopt(curl_handle, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
    /* allow whatever auth the server speaks */
    curl_easy_setopt(curl_handle, CURLOPT_HTTPAUTH, CURLAUTH_ANY);

    return curl_handle;
}

int process_html(CURL *curl_handle, RECV_BUF *p_recv_buf)
{
    int follow_relative_link = 1;
    char *url = NULL;

    curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &url);
    find_http(p_recv_buf->buf, p_recv_buf->size, follow_relative_link, url);
    return 0;
}

int process_png(CURL *curl_handle, RECV_BUF *p_recv_buf)
{
    pthread_mutex_lock(&count);
    if (img_count < m)
    {
        img_count += 1;
    }
    else
    {
        pthread_mutex_unlock(&count);
        return 0;
    }
    pthread_mutex_unlock(&count);
    char *eurl = NULL; /* effective URL */
    curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &eurl);
    if (eurl != NULL)
    {
        FILE *fp = fopen("png_urls.txt", "a");
        fwrite(eurl, strlen(eurl), 1, fp); // write png
        fwrite("\n", sizeof(char), 1, fp); // write new line
        fclose(fp);
    }

    return 0;
}
/**
 * @brief process teh download data by curl
 * @param CURL *curl_handle is the curl handler
 * @param RECV_BUF p_recv_buf contains the received data.
 * @return 0 on success; non-zero otherwise
 */

int process_data(CURL *curl_handle, RECV_BUF *p_recv_buf, char *curr_url)
{
    CURLcode res;
    long response_code;
    char *eurl = NULL;

    res = curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &eurl); // get last used URL in case of redirects

    pthread_mutex_lock(&frontier);
    if (hinsert(eurl) == 1) // add effective url to log file if doesn't exist since top level url is already in
    {
        if (log_file == 1)
        { // add effective to log file
            FILE *fp = fopen(v, "a");
            fwrite(eurl, strlen(eurl), 1, fp);
            fwrite("\n", sizeof(char), 1, fp);
            fclose(fp);
        }
    }
    else if (strcmp(eurl, curr_url) != 0)
    { // current is not effective url, move onto next link in the list
        pthread_mutex_unlock(&frontier);
        return 0;
    }
    pthread_mutex_unlock(&frontier);

    if (res == CURLE_OK)
    {
        // printf("Response code: %ld\n", response_code);
    }

    if (response_code >= 400)
    {
        // fprintf(stderr, "Error.\n");
        return 1;
    }

    char *ct = NULL;
    res = curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_TYPE, &ct);
    if (res == CURLE_OK && ct != NULL)
    {
        // printf("Content-Type: %s, len=%ld\n", ct, strlen(ct));
    }
    else
    {
        // fprintf(stderr, "Failed obtain Content-Type\n");
        return 2;
    }

    if (strstr(ct, CT_HTML))
    {
        return process_html(curl_handle, p_recv_buf);
    }
    else if (strstr(ct, CT_PNG))
    {
        // check disguised pngs (lab 1 code)
        unsigned char header[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}; // png header
        unsigned char buffer[8];                                                    // buffer for file png header
        memcpy(buffer, p_recv_buf->buf, 8);
        for (int i = 0; i < 8; i++)
        {
            if (header[i] != buffer[i])
            { // doesn't match header, not png
                return 0;
            }
            return process_png(curl_handle, p_recv_buf);
        }
    }
    return 0;
}

void barrier()
{
    pthread_mutex_lock(&barrier_lock);
    barrier_count++;
    if (barrier_count < t)
    {
        pthread_cond_wait(&cv, &barrier_lock);
    }
    else
    {
        first_loop = 1;    // done first loop/thread sync thing
        barrier_count = 0; // reset for next loop to sync all threads again
        pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&barrier_lock);
}

void *crawl_web()
{
    while (1)
    {
        barrier();
        pthread_mutex_lock(&count); // check if all images have been processed
        if (img_count >= m)
        {
            pthread_mutex_unlock(&count);
            break;
        }
        pthread_mutex_unlock(&count);

        pthread_mutex_lock(&frontier);
        if (url_frontier->head == NULL && first_loop == 1)
        { // first loop is done, all threads have synced up and head is still NULL, break since no more entries
            pthread_mutex_unlock(&frontier);
            break;
        }

        first_loop = 0; // reset first counter if not empty in case only one item in queue

        if (url_frontier->head == NULL) // if list is empty, continue to next iteration
        {
            pthread_mutex_unlock(&frontier);
            continue;
        }

        node *temp = url_frontier->head;
        char *curr_url = malloc(strlen(temp->url) + 1);
        memcpy(curr_url, temp->url, strlen(temp->url) + 1);
        url_frontier->head = temp->next; // move head of list to next, is fine if it's null;

        free(temp->url);
        free(temp);

        pthread_mutex_unlock(&frontier);

        CURL *curl_handle;
        CURLcode res;
        RECV_BUF recv_buf;

        curl_handle = easy_handle_init(&recv_buf, curr_url);

        if (curl_handle == NULL)
        {
            // fprintf(stderr, "Curl initialization failed. Exiting...\n");
            curl_global_cleanup();
            abort();
        }

        res = curl_easy_perform(curl_handle);

        if (res != CURLE_OK)
        {
            // fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            cleanup(curl_handle, &recv_buf);
        }
        else
        {
            /* process the download data */
            process_data(curl_handle, &recv_buf, curr_url);
            cleanup(curl_handle, &recv_buf);
        }
        free(curr_url);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int c;
    t = 1;
    m = 50;
    first_loop = 0;
    char *url;
    char *str = "option requires an argument";
    url_frontier = malloc(sizeof(linked_list));
    curl_global_init(CURL_GLOBAL_DEFAULT);

    while ((c = getopt(argc, argv, "t:m:v:")) != -1)
    {
        switch (c)
        {
        case 't':
            t = strtoul(optarg, NULL, 10);
            if (t <= 0)
            {
                fprintf(stderr, "%s: %s input must be > 0 -- 't'\n", argv[0], str);
                exit(-1);
            }
            break;
        case 'm':
            m = strtoul(optarg, NULL, 10);
            if (m <= 0)
            {
                fprintf(stderr, "%s: %s input must be > 0 -- 'm'\n", argv[0], str);
                exit(-1);
            }
            break;
        case 'v':
            memcpy(v, optarg, strlen(optarg) + 1);
            log_file = 1;
            break;

        default:
            exit(-1);
        }
    }

    if (optind < argc)
    {
        url = argv[optind];
    }

    hcreate(10000);                        // create hashtable
    FILE *fp = fopen("png_urls.txt", "w"); // create pngurls or empty if exists
    fclose(fp);

    if (log_file == 1)
    {
        FILE *fp = fopen(v, "w"); // create log_file or empty if exists
        fclose(fp);
    }

    if (hinsert(url) == 1)
    {                                              // inserted into hash
        url_frontier->head = malloc(sizeof(node)); // add to url frontier
        node *temp = url_frontier->head;
        temp->next = NULL;
        temp->url = malloc(strlen(url) + 1);
        memcpy(temp->url, url, strlen(url) + 1);

        if (log_file == 1)
        {
            fp = fopen(v, "a");
            fwrite(url, strlen(url), 1, fp);
            fwrite("\n", sizeof(char), 1, fp);
            fclose(fp);
        }
    }

    pthread_t *p_tids = malloc(sizeof(pthread_t) * t);
    pthread_mutex_init(&frontier, NULL);
    pthread_mutex_init(&count, NULL);
    pthread_mutex_init(&barrier_lock, NULL);
    pthread_cond_init(&cv, NULL);

    double times[2];
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
    {
        perror("gettimeofday");
        abort();
    }
    times[0] = (tv.tv_sec) + tv.tv_usec / 1000000.;

    for (int i = 0; i < t; i++)
    {
        int check = pthread_create(p_tids + i, NULL, crawl_web, NULL);
        if (check != 0)
        {
            printf("Error in creation of thread: %d\nError code: %d\n", i, check);
            exit(1);
        }
    }
    for (int i = 0; i < t; i++)
    {
        pthread_join(p_tids[i], NULL);
    }

    if (gettimeofday(&tv, NULL) != 0)
    {
        perror("gettimeofday");
        abort();
    }

    times[1] = (tv.tv_sec) + tv.tv_usec / 1000000.;
    printf("findpng2 execution time: %.6lf seconds\n", times[1] - times[0]);

    node *temp = url_frontier->head;
    while (temp != NULL)
    {
        node *temp2 = temp;
        temp = temp->next;
        free(temp2->url);
        free(temp2);
    }
    url_frontier->head = NULL;
    free(url_frontier);
    free(p_tids);
    pthread_mutex_destroy(&frontier);
    pthread_mutex_destroy(&count);
    pthread_mutex_destroy(&barrier_lock);
    pthread_cond_destroy(&cv);
    hdestroy();
    xmlCleanupParser();
    curl_global_cleanup();
    return 0;
}
