#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")


typedef SOCKET SocketType;
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <time.h>

pthread_mutex_t peerLock = PTHREAD_MUTEX_INITIALIZER;


typedef int SocketType;
#define CLOSE_SOCKET close
#define INVALID_SOCKET -1
#endif

#ifdef _WIN32
#define SLEEP(x) Sleep((DWORD)((x) * 1000))
#else
#define SLEEP(x) usleep((useconds_t)((x) * 1000000))
#endif

#include <ctype.h>

char myMAC[32];
char myIP[64];
char myName[64];
char myTimeStr[64];
long long myTotal;
long long myAvail;

#define FILE_PORT 44678
#define DISCOVERY_PORT 44679
#define FILE_REQUEST_PORT 44680
#define BUFFER_SIZE 4096
#define MAX_PEERS 50
#define PEER_FILE "peers.txt"
#define MAX_FILES 20
#define MAX_FILENAME 128

void getCurrentTime(char *buffer, size_t size);
time_t convertToTimeT(const char *timestamp);
void sortPeers();
int compareResponses(const void *a, const void *b);
void broadcastUpdateLine(const char *myTimeStr, const char *myMAC, const char *myIP,
                         const char *myName, long long myTotal, long long myAvail,
                         int filecount, char filenames[][128], int PORT);
int loadPeersFromTmp(const char *file);
void addOrUpdatePeers(const char *timeStr, const char *mac, const char *ip,
                      const char *name, long long total, long long avail,
                      int fileCount, char files[][128]);

typedef struct
{
    char timeStr[64];
    char mac[32];
    char ip[64];
    char name[64];
    long long total;
    long long avail;

    int fileCount;
    char files[MAX_FILES][MAX_FILENAME];

} Peer;

typedef struct
{
    char timeStr[64];
    time_t timeT;
    char fromIP[32];
    int peercount;
} UpdateResponse;

UpdateResponse upresp[100];
int uprespCount = 0;
int waitingForResponses = 0;

Peer peerList[MAX_PEERS];
int peerCount = 0;

static ssize_t sendAll(SocketType s, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = send(s, p + sent, (int)(len - sent), 0);
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

int loadPeersFromTmp(const char *file)
{
    FILE *fp = fopen(file, "r");

    if (!fp)
    {
        printf("[Info] No temp file to merge.\n");
        return 0; 
    }

    char line[1024];
    int addedCount = 0;

    while (fgets(line, sizeof(line), fp))
    {
        line[strcspn(line, "\n")] = 0;

        char *timeStr = strtok(line, "|");
        char *mac = strtok(NULL, "|");
        char *ip = strtok(NULL, "|");
        char *name = strtok(NULL, "|");
        char *totalStr = strtok(NULL, "|");
        char *availStr = strtok(NULL, "|");
        char *fileList = strtok(NULL, "|");

        if (!mac || !ip)
            continue;

        long long total = totalStr ? atoll(totalStr) : 0;
        long long avail = availStr ? atoll(availStr) : 0;

        char filenames[MAX_FILES][128];
        memset(filenames, 0, sizeof(filenames));
        int filecount = 0;

        if (fileList && strlen(fileList) > 0)
        {
            char fileListCopy[512];
            strncpy(fileListCopy, fileList, sizeof(fileListCopy) - 1);
            fileListCopy[sizeof(fileListCopy) - 1] = '\0';

            char *tok = strtok(fileListCopy, ",");
            while (tok && filecount < MAX_FILES)
            {
                strncpy(filenames[filecount], tok, MAX_FILENAME - 1);
                filenames[filecount][MAX_FILENAME - 1] = '\0';
                filecount++;
                tok = strtok(NULL, ",");
            }
        }

      
        addOrUpdatePeers(timeStr, mac, ip, name, total, avail, filecount, filenames);
        addedCount++;
    }

    fclose(fp);
   
    return 1; 
}

void sleep_ms(int milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
#endif
}

void getMyMAC(char *macOut)
{
#ifdef _WIN32
    FILE *fp = _popen("getmac /fo csv /nh", "r");
    if (!fp)
    {
        strcpy(macOut, "00:00:00:00:00:00");
        return;
    }

    char line[256];
    fgets(line, sizeof(line), fp);
    _pclose(fp);

    int i, j = 0;
    for (i = 0; line[i] != '\0' && j < 17; i++)
    {
        if (isxdigit((unsigned char)line[i]) || line[i] == '-' || line[i] == ':')
        {
            char c = line[i];
            if (c == '-')
                c = ':'; 
            macOut[j++] = tolower((unsigned char)c);
        }
    }
    macOut[j] = '\0';

#else
    FILE *fp = popen(
        "ifconfig en0 2>/dev/null | grep ether | awk '{print $2}' || "
        "ifconfig eth0 2>/dev/null | grep ether | awk '{print $2}' || "
        "ifconfig wlan0 2>/dev/null | grep ether | awk '{print $2}'",
        "r");

    if (!fp)
    {
        strcpy(macOut, "00:00:00:00:00:00");
        return;
    }

    char line[256];
    if (fgets(line, sizeof(line), fp))
    {
      
        int j = 0;
        for (int i = 0; line[i] != '\0' && j < 17; i++)
        {
            if (isxdigit((unsigned char)line[i]) || line[i] == ':')
            {
                macOut[j++] = tolower((unsigned char)line[i]);
            }
        }
        macOut[j] = '\0';
    }
    else
    {
        strcpy(macOut, "00:00:00:00:00:00");
    }

    pclose(fp);
#endif
}

void getMyIP(char *ipOut)
{
#ifdef _WIN32
    char hostname[256];
    struct hostent *host;

    if (gethostname(hostname, sizeof(hostname)) == 0)
    {
        host = gethostbyname(hostname);
        if (host != NULL && host->h_addr_list[0] != NULL)
        {
            struct in_addr addr;
            memcpy(&addr.s_addr, host->h_addr_list[0], sizeof(addr.s_addr));
            strcpy(ipOut, inet_ntoa(addr));
            return;
        }
    }
    strcpy(ipOut, "127.0.0.1");
#else
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        strcpy(ipOut, "127.0.0.1");
        return;
    }

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &server.sin_addr);

    connect(sock, (struct sockaddr *)&server, sizeof(server));

    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(sock, (struct sockaddr *)&local, &len);
    inet_ntop(AF_INET, &local.sin_addr, ipOut, 64);

    close(sock);
#endif
}

void getMyName(char *nameOut)
{
#ifdef _WIN32
    DWORD size = 256;
    GetComputerNameA(nameOut, &size);
#else
    gethostname(nameOut, 256);
#endif
}


void loadPeers(const char *file)
{
    FILE *fp = fopen(file, "r");
    peerCount = 0;

    if (!fp)
    {
        printf("[Info] No saved peers yet.\n");
        return;
    }

    char line[1024];

    while (fgets(line, sizeof(line), fp))
    {
        line[strcspn(line, "\n")] = 0;

        char *timeStr = strtok(line, "|");
        char *mac = strtok(NULL, "|");
        char *ip = strtok(NULL, "|");
        char *name = strtok(NULL, "|");
        char *totalStr = strtok(NULL, "|");
        char *availStr = strtok(NULL, "|");
        char *fileList = strtok(NULL, "|");

        if (!mac || !ip)
            continue;

        Peer *p = &peerList[peerCount];

        strncpy(p->timeStr, timeStr ? timeStr : "", sizeof(p->timeStr) - 1);
        p->timeStr[sizeof(p->timeStr) - 1] = '\0';

        strncpy(p->mac, mac, sizeof(p->mac) - 1);
        p->mac[sizeof(p->mac) - 1] = '\0';

        strncpy(p->ip, ip, sizeof(p->ip) - 1);
        p->ip[sizeof(p->ip) - 1] = '\0';

        strncpy(p->name, name ? name : "", sizeof(p->name) - 1);
        p->name[sizeof(p->name) - 1] = '\0';

        p->total = totalStr ? atoll(totalStr) : 0;
        p->avail = availStr ? atoll(availStr) : 0;

        p->fileCount = 0;

        if (fileList && strlen(fileList) > 0)
        {
            
            char fileListCopy[512];
            strncpy(fileListCopy, fileList, sizeof(fileListCopy) - 1);
            fileListCopy[sizeof(fileListCopy) - 1] = '\0';

            char *tok = strtok(fileListCopy, ",");
            while (tok && p->fileCount < MAX_FILES)
            {
                strncpy(p->files[p->fileCount], tok, MAX_FILENAME - 1);
                p->files[p->fileCount][MAX_FILENAME - 1] = '\0';
                p->fileCount++;
                tok = strtok(NULL, ",");
            }
        }

        peerCount++;
        if (peerCount >= MAX_PEERS)
            break;
    }

    fclose(fp);
    printf("[Loaded] %d peer(s)\n", peerCount);
}

void savePeers(const char *file)
{
    FILE *fp = fopen(file, "w");
    if (!fp)
    {
        printf("[Error] Could not open file for writing: %s\n", file);
        return;
    }

    for (int i = 0; i < peerCount; i++)
    {
        Peer *p = &peerList[i];

        fprintf(fp, "%s|%s|%s|%s|%lld|%lld|",
                p->timeStr,
                p->mac,
                p->ip,
                p->name,
                p->total,
                p->avail);

        if (p->fileCount > 0)
        {
            for (int f = 0; f < p->fileCount; f++)
            {
                fprintf(fp, "%s", p->files[f]);
                if (f < p->fileCount - 1)
                    fprintf(fp, ",");
            }
        }

        fprintf(fp, "\n");
    }

    fclose(fp);
}

void addOrUpdatePeers(const char *timeStr, const char *mac, const char *ip,
                      const char *name, long long total, long long avail,
                      int fileCount, char files[][128])
{
    if (!mac || !ip)
        return;

   
    if (fileCount > MAX_FILES)
        fileCount = MAX_FILES;

    for (int i = 0; i < peerCount; i++)
    {
        Peer *p = &peerList[i];

        if (strcmp(p->mac, mac) == 0)
        {
            strncpy(p->ip, ip, sizeof(p->ip) - 1);
            p->ip[sizeof(p->ip) - 1] = '\0';

            strncpy(p->timeStr, timeStr ? timeStr : "", sizeof(p->timeStr) - 1);
            p->timeStr[sizeof(p->timeStr) - 1] = '\0';

            strncpy(p->name, name ? name : "", sizeof(p->name) - 1);
            p->name[sizeof(p->name) - 1] = '\0';

            p->total = total;
            p->avail = avail;

            p->fileCount = fileCount;
            for (int k = 0; k < fileCount; k++)
            {
                strncpy(p->files[k], files[k], MAX_FILENAME - 1);
                p->files[k][MAX_FILENAME - 1] = '\0';
            }

            sortPeers();
            savePeers("peers.txt");
            return;
        }
    }

    if (peerCount < MAX_PEERS)
    {
        Peer *p = &peerList[peerCount];

        strncpy(p->timeStr, timeStr ? timeStr : "", sizeof(p->timeStr) - 1);
        p->timeStr[sizeof(p->timeStr) - 1] = '\0';

        strncpy(p->mac, mac, sizeof(p->mac) - 1);
        p->mac[sizeof(p->mac) - 1] = '\0';

        strncpy(p->ip, ip, sizeof(p->ip) - 1);
        p->ip[sizeof(p->ip) - 1] = '\0';

        strncpy(p->name, name ? name : "", sizeof(p->name) - 1);
        p->name[sizeof(p->name) - 1] = '\0';

        p->total = total;
        p->avail = avail;

        p->fileCount = fileCount;
        for (int k = 0; k < fileCount; k++)
        {
            strncpy(p->files[k], files[k], MAX_FILENAME - 1);
            p->files[k][MAX_FILENAME - 1] = '\0';
        }

        peerCount++;
        sortPeers();
        savePeers("peers.txt");
    }
    else
    {
        printf("[Warning] Maximum peers reached (%d). Cannot add new peer.\n", MAX_PEERS);
    }
}

char *findIPByMAC(const char *mac)
{
    for (int i = 0; i < peerCount; i++)
    {
        if (strcmp(peerList[i].mac, mac) == 0)
        {
            return peerList[i].ip;
        }
    }
    return NULL;
}

void showPeers()
{
    if (peerCount == 0)
    {
        printf("\n  No peers found yet!\n");
        printf("  Try the 'discover' command first.\n\n");
        return;
    }

    printf("\n  ========== Known Peers (%d) ==========\n", peerCount);
    for (int i = 0; i < peerCount; i++)
    {
        Peer *p = &peerList[i];

        printf("  [%d] %s\n", i + 1, p->name);
        printf("      IP:    %s\n", p->ip);
        printf("      MAC:   %s\n", p->mac);
        printf("      Time:  %s\n", p->timeStr);
        printf("      TOTAL: %lld MB\n", p->total / (1024 * 1024));
        printf("      AVAIL: %lld MB\n", p->avail / (1024 * 1024));

        if (p->fileCount == 0)
        {
            printf("      Files: None\n");
        }
        else
        {
            printf("      Files:\n");
            for (int f = 0; f < p->fileCount; f++)
            {
                printf("        - %s\n", p->files[f]);
            }
        }

        printf("\n");
    }
}

char *findPeer(const char *target)
{
    if (!target)
        return NULL;

    int num = atoi(target);
    if (num > 0 && num <= peerCount)
    {
        return peerList[num - 1].ip;
    }

    if (strchr(target, '.'))
    {
        return (char *)target;
    }

    if (strchr(target, ':'))
    {
        return findIPByMAC(target);
    }

    for (int i = 0; i < peerCount; i++)
    {
        if (strcmp(peerList[i].name, target) == 0)
        {
            return peerList[i].ip;
        }
    }

    return NULL;
}

int comparePeersByTime(const void *a, const void *b)
{
    const Peer *pa = (const Peer *)a;
    const Peer *pb = (const Peer *)b;

    time_t ta = convertToTimeT(pa->timeStr);
    time_t tb = convertToTimeT(pb->timeStr);


    if (ta > tb)
        return -1;
    if (ta < tb)
        return 1;
    return 0;
}
void sortPeers()
{
    qsort(peerList, peerCount, sizeof(Peer), comparePeersByTime);
}


void addFileToPeer(Peer *p, const char *filename)
{
    if (p == NULL || filename == NULL)
        return;

    if (p->fileCount >= MAX_FILES)
    {
        printf("Cannot add more files. MAX_FILES reached.\n");
        return;
    }

    strncpy(p->files[p->fileCount], filename, MAX_FILENAME - 1);
    p->files[p->fileCount][MAX_FILENAME - 1] = '\0';
    p->fileCount++;
}

void getPeersSortedByAvail(int sorted[], int count)
{
    for (int i = 0; i < count; i++)
        sorted[i] = i;

    for (int i = 0; i < count - 1; i++)
    {
        for (int j = i + 1; j < count; j++)
        {
            if (peerList[sorted[j]].avail > peerList[sorted[i]].avail)
            {
                int temp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = temp;
            }
        }
    }
}

long long getFileSize(const char *filepath)
{
#ifdef _WIN32
    
    struct stat st;

    if (stat(filepath, &st) != 0)
    {
        return -1;
    }

    return (long long)st.st_size;
#else
  
    struct stat st;

    if (stat(filepath, &st) != 0)
    {
        return -1;
    }

    return (long long)st.st_size;
#endif
}

int recvAll(SocketType sock, char *buffer, int length)
{
    int totalReceived = 0;
    while (totalReceived < length)
    {
        int bytes = recv(sock, buffer + totalReceived, length - totalReceived, 0);
        if (bytes <= 0)
        {
            return -1;
        }
        totalReceived += bytes;
    }
    return totalReceived;
}

void receiveFile(SocketType clientSocket, const char *senderIP)
{
    char buffer[BUFFER_SIZE];

    

    
    uint32_t infoLen;
    if (recvAll(clientSocket, (char *)&infoLen, sizeof(infoLen)) < 0)
    {
        printf("    [Error] Failed to receive info length\n");
        return;
    }
  
    infoLen = ntohl(infoLen); 
   

    // Safety check
    if (infoLen > 1024)
    {
        printf("    [Error] Invalid info length: %u\n", infoLen);
        return;
    }

    char info[1024];
    if (recvAll(clientSocket, info, infoLen) < 0)
    {
        printf("    [Error] Failed to receive peer info\n");
        return;
    }
    info[infoLen] = 0;
   

    char infoCopy[1024];
    strcpy(infoCopy, info);
    char *mac = strtok(infoCopy, "|");
    char *ip = strtok(NULL, "|");
    char *name = strtok(NULL, "|");

    uint32_t nameLen;
    if (recvAll(clientSocket, (char *)&nameLen, sizeof(nameLen)) < 0)
    {
        printf("    [Error] Failed to receive filename length\n");
        return;
    }
   
    nameLen = ntohl(nameLen);
   

    if (nameLen > 1024)
    {
        printf("    [Error] Invalid filename length: %u\n", nameLen);
        return;
    }

    char filename[1024];
    if (recvAll(clientSocket, filename, nameLen) < 0)
    {
        printf("    [Error] Failed to receive filename\n");
        return;
    }
    filename[nameLen] = 0;
  

    FILE *file = fopen(filename, "wb");
    if (!file)
    {
        printf("    [Error] Cannot create file: %s\n", filename);
        return;
    }

    int totalBytes = 0;
    int bytes;
  
    fflush(stdout);
    int flag =0;
    while ((bytes = recv(clientSocket, buffer, BUFFER_SIZE, 0)) > 0)
    {
        fwrite(buffer, 1, bytes, file);
        totalBytes += bytes;
        if(flag==100){
        printf(".");
        flag=0;
        }

        flag++;
        fflush(stdout);
    }

    fclose(file);
    printf(" Complete! (%d bytes)\n", totalBytes);
}
void findBestPeer(char **bestIP, int *peerIndex)
{
    if (bestIP)
        *bestIP = NULL;
    if (peerIndex)
        *peerIndex = -1;

    if (peerCount == 0)
    {
        printf("    [Error] No peers in network.\n");
        return;
    }

    char myMAC[32];
    getMyMAC(myMAC);

    int order[MAX_PEERS];
    getPeersSortedByAvail(order, peerCount);

    int flag = 0;
    int i;
    for (int r = 0; r < peerCount; r++)
    {
        i = order[r];
        char *ip = peerList[i].ip;

        printf("    [Checking] %s (%s) - %lld MB available\n",
               peerList[i].name, ip, peerList[i].avail / (1024 * 1024));

       
        if (strcmp(peerList[i].mac, myMAC) == 0)
        {
            printf("    [Note] This is your machine\n");

           
            if (bestIP && *bestIP == NULL)
            {
                *bestIP = ip;
                if (peerIndex)
                    *peerIndex = i;
            }
            flag = 1;
            break;
        }

        
        SocketType testSock = socket(AF_INET, SOCK_STREAM, 0);
        if (testSock == INVALID_SOCKET)
            continue;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(FILE_PORT);

#ifdef _WIN32
        addr.sin_addr.s_addr = inet_addr(ip);
#else
        inet_pton(AF_INET, ip, &addr.sin_addr);
#endif

        if (connect(testSock, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        {
           
            CLOSE_SOCKET(testSock);

            if (bestIP)
                *bestIP = ip;
            if (peerIndex)
                *peerIndex = i;

            return; 
        }

        CLOSE_SOCKET(testSock);
        printf("    [Offline] %s is not reachable\n", peerList[i].name);
    }

    if (flag == 1)
    {
        *bestIP = myIP;
        *peerIndex = i;
        return;
    }

    if (bestIP && *bestIP != NULL)
    {
        printf("    [Fallback] No remote peers available, using local storage\n");
    }
    else
    {
        printf("    [Error] No available peers found\n");
    }
}

void sendFile(const char *filepath, const char *targetPeer)
{
    char *ip = findPeer(targetPeer);
    if (!ip)
    {
        printf("    [Error] Peer not found!\n");
        printf("    Use 'list' to see available peers.\n");
        return;
    }

    FILE *file = fopen(filepath, "rb");
    if (!file)
    {
        printf("    [Error] File not found: %s\n\n", filepath);
        printf("    TIPS:\n");
        printf("    - On Windows, include the drive letter: C:\\Users\\...\n");
        printf("    - Or use forward slashes: C:/Users/...\n");
        printf("    - Use quotes if path has spaces: \"C:/My Files/photo.jpg\"\n");
        printf("    - Or drag & drop the file into this window!\n\n");
        return;
    }

    SocketType sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(FILE_PORT);

#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr(ip);
#else
    inet_pton(AF_INET, ip, &addr.sin_addr);
#endif

    printf("    [Sending] Connecting to %s...\n", ip);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        printf("    [Error] Cannot connect to %s\n", ip);
        printf("    Make sure they're running the program!\n");
        fclose(file);
        CLOSE_SOCKET(sock);
        return;
    }

    
    char myMAC[32], myIP[64], myName[256];
    getMyMAC(myMAC);
    getMyIP(myIP);
    getMyName(myName);

    char info[256];
    snprintf(info, sizeof(info), "%s|%s|%s", myMAC, myIP, myName);
    uint32_t infoLen = htonl(strlen(info)); 
    send(sock, (char *)&infoLen, sizeof(infoLen), 0);
    send(sock, info, ntohl(infoLen), 0);

    char *filename = strrchr(filepath, '/');
    if (!filename)
        filename = strrchr(filepath, '\\');
    filename = filename ? filename + 1 : (char *)filepath;

    uint32_t nameLen = htonl(strlen(filename));
    send(sock, (char *)&nameLen, sizeof(nameLen), 0);
    send(sock, filename, ntohl(nameLen), 0);

    char buffer[BUFFER_SIZE];
    int bytes;
    int totalBytes = 0;

    printf("    [Sending] %s", filename);
    fflush(stdout);
    int flag =0;
    while ((bytes = fread(buffer, 1, BUFFER_SIZE, file)) > 0)
    {
        send(sock, buffer, bytes, 0);
        totalBytes += bytes;
        if(flag==100){
        printf(".");
        flag=0;
        }
        fflush(stdout);
        flag++;
    }

    printf(" Complete! (%d bytes)\n", totalBytes);

    fclose(file);
    CLOSE_SOCKET(sock);
}

void sendTxtFile(const char *filepath, const char *targetPeer)
{
    char *ip = findPeer(targetPeer);
    if (!ip)
    {
        printf("    [Error] Peer not found!\n");
        printf("    Use 'list' to see available peers.\n");
        return;
    }

    FILE *file = fopen(filepath, "rb");
    if (!file)
    {
        printf("    [Error] File not found: %s\n\n", filepath);
        printf("    TIPS:\n");
        printf("    - On Windows, include the drive letter: C:\\Users\\...\n");
        printf("    - Or use forward slashes: C:/Users/...\n");
        printf("    - Use quotes if path has spaces: \"C:/My Files/photo.jpg\"\n");
        printf("    - Or drag & drop the file into this window!\n\n");
        return;
    }

    SocketType sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(FILE_PORT);

#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr(ip);
#else
    inet_pton(AF_INET, ip, &addr.sin_addr);
#endif

    printf("    [Sending] Connecting to %s...\n", ip);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        printf("    [Error] Cannot connect to %s\n", ip);
        printf("    Make sure they're running the program!\n");
        fclose(file);
        CLOSE_SOCKET(sock);
        return;
    }

    char myMAC[32], myIP[64], myName[256];
    getMyMAC(myMAC);
    getMyIP(myIP);
    getMyName(myName);

    char info[256];
    snprintf(info, sizeof(info), "%s|%s|%s", myMAC, myIP, myName);
    uint32_t infoLen = htonl(strlen(info)); 
    send(sock, (char *)&infoLen, sizeof(infoLen), 0);
    send(sock, info, ntohl(infoLen), 0);

   const char *filename = "peers.tmp";

  
    uint32_t nameLen = htonl(strlen(filename));
    send(sock, (char *)&nameLen, sizeof(nameLen), 0);
    send(sock, filename, ntohl(nameLen), 0);

   
    char buffer[BUFFER_SIZE];
    int bytes;
    int totalBytes = 0;

    printf("    [Sending] %s", filename);
    fflush(stdout);
    int flag = 0;
    while ((bytes = fread(buffer, 1, BUFFER_SIZE, file)) > 0)
    {
        send(sock, buffer, bytes, 0);
        totalBytes += bytes;
        if(flag==100){
        printf(".");
        flag=0;
        }
        flag ++;
        fflush(stdout);
    }

    printf(" Complete! (%d bytes)\n", totalBytes);

    fclose(file);
    CLOSE_SOCKET(sock);
}


#ifdef _WIN32
DWORD WINAPI fileServerThread(LPVOID param)
{
#else
void *fileServerThread(void *param)
{
#endif
    SocketType serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    int reuse = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(FILE_PORT);

    if (bind(serverSocket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        printf("  [Error] Cannot start file server!\n");
        return 0;
    }

    listen(serverSocket, 5);
    

    while (1)
    {
        struct sockaddr_in clientAddr;
        socklen_t len = sizeof(clientAddr);

        SocketType clientSocket = accept(serverSocket, (struct sockaddr *)&clientAddr, &len);

        char *senderIP = inet_ntoa(clientAddr.sin_addr);
        printf("\n  [Connection] From %s\n", senderIP);

        receiveFile(clientSocket, senderIP);
        CLOSE_SOCKET(clientSocket);

        printf("\n> ");
        fflush(stdout);
    }

    return 0;
}

void deleteTempFile(const char *filename)
{
    if (remove(filename) == 0)
    {
        
    }
    else
    {
        perror("Error deleting temp file");
    }
}

void sendUnicastMessage(const char *ip, const char *msg, int PORT)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr(ip);
#else
    inet_pton(AF_INET, ip, &addr.sin_addr);
#endif

    sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&addr, sizeof(addr));
    CLOSE_SOCKET(sock);
}

void broadcastMessage(const char *msg, int PORT)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return;
    int yes = 1;

    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_BROADCAST;

    sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&addr, sizeof(addr));
    CLOSE_SOCKET(sock);
}

time_t convertToTimeT(const char *timestamp)
{
    struct tm t;
    memset(&t, 0, sizeof(t));

    if (!timestamp)
        return (time_t)0;

    if (sscanf(timestamp, "%d-%d-%d %d:%d:%d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec) != 6)
    {
        return (time_t)0;
    }

    t.tm_year -= 1900; 
    t.tm_mon -= 1;     

    return mktime(&t);
}

char *getWinnerIP()
{
    static char winnerIP[32];
    memset(winnerIP, 0, sizeof(winnerIP));

    if (peerCount == 0)
    {
        printf("[Sync] No peers to sync with\n");
        return NULL;
    }

    memset(upresp, 0, sizeof(upresp));
    uprespCount = 0;
    waitingForResponses = 1;

    char final[256];
    snprintf(final, sizeof(final), "%s|%s|%d", "SYNC", peerList[peerCount - 1].timeStr, peerCount);
    broadcastMessage(final, DISCOVERY_PORT);

    printf("[Sync] Waiting 500ms...\n");
    sleep_ms(500);
    waitingForResponses = 0;

    if (uprespCount == 0)
    {
        printf("[Sync] No responses\n");
        return NULL;
    }

    qsort(upresp, uprespCount, sizeof(UpdateResponse), compareResponses);

    
    strncpy(winnerIP, upresp[0].fromIP, sizeof(winnerIP) - 1);
    printf("[Winner] %s\n", winnerIP);

    return winnerIP;
}

int compareResponses(const void *a, const void *b)
{
    const UpdateResponse *respA = (const UpdateResponse *)a;
    const UpdateResponse *respB = (const UpdateResponse *)b;

    if (respA->peercount != respB->peercount)
        return respB->peercount - respA->peercount;

    if (respA->timeT != respB->timeT)
        return (respA->timeT > respB->timeT) ? -1 : 1;

    return 0;
}

void broadcastUpdateLine(
    const char *myTimeStr_,
    const char *myMAC_,
    const char *myIP_,
    const char *myName_,
    long long myTotal_,
    long long myAvail_,
    int filecount,
    char filenames[][MAX_FILENAME],
    int PORT)
{
    if (!myTimeStr_ || !myMAC_ || !myIP_ || !myName_)
        return;

    if (filecount < 0)
        filecount = 0;
    if (filecount > MAX_FILES)
        filecount = MAX_FILES;

   
    size_t approx = 512 + (size_t)filecount * (MAX_FILENAME + 4);
    char *msg = (char *)malloc(approx);
    if (!msg)
        return;
    msg[0] = '\0';

    size_t used = 0;
    int n = snprintf(msg + used, approx - used,
                     "UPDATE_LINE|%s|%s|%s|%s|%lld|%lld|%d",
                     myTimeStr_, myMAC_, myIP_, myName_, myTotal_, myAvail_, filecount);
    if (n < 0 || (size_t)n >= approx - used)
    {
        free(msg);
        return;
    }
    used += (size_t)n;

    for (int i = 0; i < filecount && i < MAX_FILES; ++i)
    {
      
        filenames[i][MAX_FILENAME - 1] = '\0';
        n = snprintf(msg + used, approx - used, "|%s", filenames[i]);
        if (n < 0)
            break;
        if ((size_t)n >= approx - used)
            break; 
        used += (size_t)n;
    }


    broadcastMessage(msg, PORT);

    free(msg);
}

void handleIncomingMessage(const char *fromIP, char *message)
{
    char *command = strtok(message, "|");

    if (!command)
        return;
    if (strcmp(myIP, fromIP) == 0)
    {
        return;
    }


    if (strcmp(command, "I_AM_NEW") == 0)
    {
        char *timeStr = strtok(NULL, "|");
        char *mac = strtok(NULL, "|");
        char *ip = strtok(NULL, "|");
        char *name = strtok(NULL, "|");
        char *totalStr = strtok(NULL, "|");
        char *availStr = strtok(NULL, "|");

        if (mac && ip && name && totalStr && availStr)
        {
            long long total = atoll(totalStr);
            long long avail = atoll(availStr);
            char emptyFiles[1][128] = {""};
            addOrUpdatePeers(timeStr, mac, ip, name, total, avail, 0, emptyFiles);
        }

        return;
    }

    if (strcmp(command, "HAVE_UPDATED_ONE") == 0)
    {
        if (!waitingForResponses || uprespCount >= 100)
            return;

        char *time = strtok(NULL, "|");
        char *srcIP = strtok(NULL, "|");
        char *peercount = strtok(NULL, "|");

        if (!time || !srcIP || !peercount)
            return;

        strncpy(upresp[uprespCount].timeStr, time, sizeof(upresp[uprespCount].timeStr) - 1);
        upresp[uprespCount].timeT = convertToTimeT(time);
        strncpy(upresp[uprespCount].fromIP, srcIP, sizeof(upresp[uprespCount].fromIP) - 1);
        upresp[uprespCount].peercount = atoi(peercount);
        uprespCount++;

        printf("[Response] From %s\n", srcIP);
        return;
    }

    if (strcmp(command, "UPDATE_LINE") == 0)
    {
        char *time = strtok(NULL, "|");
        char *mac = strtok(NULL, "|");
        char *ip = strtok(NULL, "|");
        char *name = strtok(NULL, "|");
        char *totalStr = strtok(NULL, "|");
        char *availStr = strtok(NULL, "|");
        char *filecountStr = strtok(NULL, "|");

        if (!time || !mac || !ip || !name || !totalStr || !availStr || !filecountStr)
            return;

        long long total = atoll(totalStr);
        long long avail = atoll(availStr);
        int filecount = atoi(filecountStr);

        char filenames[MAX_FILES][128];
        memset(filenames, 0, sizeof(filenames));

        for (int i = 0; i < filecount && i < MAX_FILES; i++)
        {
            char *f = strtok(NULL, "|");
            if (f)
            {
                strncpy(filenames[i], f, 127);
                filenames[i][127] = '\0';
            }
        }

        addOrUpdatePeers(time, mac, ip, name, total, avail, filecount, filenames);
        return;
    }

    if (strcmp(command, "I_NEED_PEERSTXT") == 0)
    {
        sendTxtFile("peers.txt", fromIP);
        return;
    }

    if (strcmp(command, "SYNC") == 0)
    {
        char *time = strtok(NULL, "|");
        char *peercountStr = strtok(NULL, "|");

        if (!time || !peercountStr)
            return;

        int peercountRequest = atoi(peercountStr);

        if (peerCount == 0)
            return;

        char tempMyLastTime[64];
        strncpy(tempMyLastTime, peerList[peerCount - 1].timeStr, sizeof(tempMyLastTime) - 1);
        time_t myLastTime = convertToTimeT(tempMyLastTime);
        time_t timeAsk = convertToTimeT(time);

        if (peerCount > peercountRequest || myLastTime > timeAsk)
        {
            int delay = rand() % 300; 
            sleep_ms(delay);

            char final[256];
            snprintf(final, sizeof(final), "%s|%s|%s|%d", "HAVE_UPDATED_ONE", peerList[peerCount - 1].timeStr, myIP, peerCount);
            sendUnicastMessage(fromIP, final, DISCOVERY_PORT);
        }

        return;
    }

    printf("[Warning] Unknown command received: %s\n", command);
}


#ifdef _WIN32
DWORD WINAPI MessageListener(LPVOID param)
{
#else
void *MessageListener(void *param)
{
#endif
    (void)param;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        printf("[Error] Could not create discovery socket\n");
        return 0;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(DISCOVERY_PORT);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        printf("[Error] Unable to bind listener!\n");
        CLOSE_SOCKET(sock);
        return 0;
    }

    printf("[Info] Message listener running on port %d\n", DISCOVERY_PORT);

    while (1)
    {
        char buffer[1024];
        struct sockaddr_in sender;
        socklen_t len = sizeof(sender);

        int bytes = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                             (struct sockaddr *)&sender, &len);

        if (bytes <= 0)
            continue;

        buffer[bytes] = '\0';

        char senderIP[32];
        strncpy(senderIP, inet_ntoa(sender.sin_addr), sizeof(senderIP) - 1);
        senderIP[sizeof(senderIP) - 1] = '\0';

        handleIncomingMessage(senderIP, buffer);
    }

    CLOSE_SOCKET(sock);
    return 0;
}

void requestFileFromPeer(const char *filename)
{
    SocketType sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        printf("  [Error] Cannot create socket for file request!\n");
        return;
    }
    char tmpMac[64];
    getMyMAC(tmpMac);

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast, sizeof(broadcast));

    struct sockaddr_in broadcastAddr;
    memset(&broadcastAddr, 0, sizeof(broadcastAddr));
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;
    broadcastAddr.sin_port = htons(FILE_REQUEST_PORT);

    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request), "FILE_REQUEST|%s|%s", tmpMac, filename);

    int result = sendto(sock, request, strlen(request), 0,
                        (struct sockaddr *)&broadcastAddr, sizeof(broadcastAddr));

    if (result < 0)
    {
        printf("  [Error] Failed to send file request!\n");
    }
    else
    {
        
    }

    CLOSE_SOCKET(sock);
}

#ifdef _WIN32
DWORD WINAPI FileRequest(LPVOID param)
{
#else
void *FileRequest(void *param)
{
#endif
    (void)param;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        printf("  [Error] Cannot create file-request socket!\n");
        return 0;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(FILE_REQUEST_PORT);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        printf("  [Error] Cannot start file request server!\n");
        CLOSE_SOCKET(sock);
        return 0;
    }

    char localIP[64], localMAC[32], localName[256];
    getMyIP(localIP);
    getMyMAC(localMAC);
    getMyName(localName);

    printf("  [Info] File request server listening on port %d\n", FILE_REQUEST_PORT);

    while (1)
    {
        char buffer[BUFFER_SIZE];
        struct sockaddr_in sender;
        socklen_t len = sizeof(sender);

        int bytes = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                             (struct sockaddr *)&sender, &len);

        if (bytes > 0)
        {
            buffer[bytes] = 0;

            if (strncmp(buffer, "FILE_REQUEST|", 13) == 0)
            {
            
                char bufferCopy[BUFFER_SIZE];
                strncpy(bufferCopy, buffer, sizeof(bufferCopy) - 1);
                bufferCopy[sizeof(bufferCopy) - 1] = '\0';

                char *command = strtok(bufferCopy, "|");
                char *requesterMAC = strtok(NULL, "|");
                char *filename = strtok(NULL, "|");

                if (requesterMAC && filename)
                {
                
                    char requesterIP[32];
                    strncpy(requesterIP, inet_ntoa(sender.sin_addr), sizeof(requesterIP) - 1);
                    requesterIP[sizeof(requesterIP) - 1] = '\0';

                    printf("  [Request] From %s (%s) for file: %s\n",
                           requesterIP, requesterMAC, filename);

                  
                    if (strcmp(requesterMAC, localMAC) == 0)
                    {
                        printf("  [Request] Ignoring self-request\n");
                        continue;
                    }

                    bool haveFile = false;
                    for (int i = 0; i < peerCount; i++)
                    {
                        if (strcmp(peerList[i].mac, localMAC) == 0)
                        {
                         
                            for (int f = 0; f < peerList[i].fileCount; f++)
                            {
                                if (strcmp(peerList[i].files[f], filename) == 0)
                                {
                                    haveFile = true;
                                    break;
                                }
                            }
                            break;
                        }
                    }

                    if (!haveFile)
                    {
                        printf("  [Request] Don't have file: %s\n", filename);
                        continue;
                    }

                    FILE *testFile = fopen(filename, "rb");
                    if (!testFile)
                    {
                        printf("  [Request] File not on disk: %s\n", filename);
                        continue;
                    }
                    fclose(testFile);

                  

                    sendFile(filename, requesterIP);
                }
            }
        }
    }

    CLOSE_SOCKET(sock);
    return 0;
}

void getCurrentTime(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    if (t != NULL)
    {
        strftime(buffer, size, "%Y-%m-%d %H:%M:%S", t);
    }
    else
    {
        snprintf(buffer, size, "Unknown Time");
    }
}

void sync()
{
    char *winnerIP = getWinnerIP();

    if (winnerIP == NULL)
    {
        printf("[Sync] No winner found. Cannot sync.\n");
        return;
    }

  
    sendUnicastMessage(winnerIP, "I_NEED_PEERSTXT", DISCOVERY_PORT);

   
    sleep_ms(2000);

    if (loadPeersFromTmp("peers.tmp"))
    {
        deleteTempFile("peers.tmp");
        printf("[Sync] Sync completed successfully!\n");
    }
    else
    {
        printf("[Sync] Failed to receive peers.txt\n");
    }
}

void showHelp()
{
    printf("\n  ========== COMMANDS ==========\n");
    printf("  sync  - Find peers on the network\n");
    printf("  list      - Show all available peers\n");
    printf("  upload      - Send a file to someone\n");
    printf("  help      - Show this help message\n");
    printf("  download      - download any file\n");
    printf("  quit      - Exit the program\n");
    printf("  ==============================\n\n");
}

void writeMyInfo(
    const char *s1,
    const char *s2,
    const char *s3,
    const char *s4,
    long long n1,
    long long n2)
{
    FILE *fp = fopen("myself.txt", "w");
    if (!fp)
        return;

    fprintf(fp, "%s|%s|%s|%s|%lld|%lld\n",
            s1, s2, s3, s4, n1, n2);

    fclose(fp);
}

bool loadMyInfo()
{
    FILE *fp = fopen("myself.txt", "r");
    if (!fp)
    {
        
        return false;
    }

    char line[256];
    if (!fgets(line, sizeof(line), fp))
    {
        fclose(fp);
        return false;
    }

    line[strcspn(line, "\n")] = 0;

    if (strlen(line) == 0)
    {
        fclose(fp);
        return false;
    }

    char *timeStr = strtok(line, "|");
    char *mac = strtok(NULL, "|");
    char *ip = strtok(NULL, "|");
    char *name = strtok(NULL, "|");
    char *totalStorage = strtok(NULL, "|");
    char *availableStorage = strtok(NULL, "|");

    if (!timeStr || !mac || !ip || !name || !totalStorage || !availableStorage)
    {
        fclose(fp);
        return false;
    }

    strncpy(myTimeStr, timeStr, sizeof(myTimeStr) - 1);
    myTimeStr[sizeof(myTimeStr) - 1] = '\0';
    strncpy(myMAC, mac, sizeof(myMAC) - 1);
    myMAC[sizeof(myMAC) - 1] = '\0';
    strncpy(myIP, ip, sizeof(myIP) - 1);
    myIP[sizeof(myIP) - 1] = '\0';
    strncpy(myName, name, sizeof(myName) - 1);
    myName[sizeof(myName) - 1] = '\0';
    myTotal = atoll(totalStorage);
    myAvail = atoll(availableStorage);

    fclose(fp);
    return true;
}

int main()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    getMyIP(myIP);
    getMyMAC(myMAC);
    getMyName(myName);

    printf("\n");
    printf("  ==========================================\n");
    printf("     P2P File Transfer (DP2PS DEVELOPED BY RIHAN-BSSE1630)\n");
    printf("  ==========================================\n");
    printf("  Computer: %s\n", myName);
    printf("  IP:       %s\n", myIP);
    printf("  MAC:      %s\n", myMAC);
    printf("  ==========================================\n\n");

    bool alreadyPeer = loadMyInfo();

    printf("  Starting background servers...\n");
    loadPeers("peers.txt");

#ifdef _WIN32
    CreateThread(NULL, 0, fileServerThread, NULL, 0, NULL);
    CreateThread(NULL, 0, MessageListener, NULL, 0, NULL);
    CreateThread(NULL, 0, FileRequest, NULL, 0, NULL);
#else
    pthread_t t1, t2, t4;
    pthread_create(&t1, NULL, fileServerThread, NULL);
    pthread_create(&t2, NULL, MessageListener, NULL);
    pthread_create(&t4, NULL, FileRequest, NULL);
    pthread_detach(t1);
    pthread_detach(t2);
    pthread_detach(t4);
#endif

    SLEEP(1);

    showHelp();

    if (!alreadyPeer)
    {
        char currentTime[64];
        printf("Enter how much you want to contribute (MB): ");
        long long contribution = 0;
        if (scanf("%lld", &contribution) != 1)
            contribution = 0;
        while (getchar() != '\n')
            ; 

        int filecount = 0;
        char files[MAX_FILES][128];
        memset(files, 0, sizeof(files));

        getCurrentTime(currentTime, sizeof(currentTime));
        strncpy(myTimeStr, currentTime, sizeof(myTimeStr) - 1);
        myTimeStr[sizeof(myTimeStr) - 1] = '\0';
        myTotal = contribution * 1024 * 1024;
        myAvail = contribution * 1024 * 1024;

        writeMyInfo(currentTime, myMAC, myIP, myName, myTotal, myAvail);
        addOrUpdatePeers(currentTime, myMAC, myIP, myName, myTotal, myAvail, filecount, files);
        char final[256];
        snprintf(final, sizeof(final), "%s|%s|%s|%s|%s|%lld|%lld", "I_AM_NEW", myTimeStr, myMAC, myIP, myName, myTotal, myAvail);

        broadcastMessage(final, DISCOVERY_PORT);
    }

    char cmd[128];
    while (1)
    {
        printf("> ");
        fflush(stdout);

        if (!fgets(cmd, sizeof(cmd), stdin))
            break;
        cmd[strcspn(cmd, "\n")] = 0;

        if (strlen(cmd) == 0)
            continue;

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0)
        {
            printf("\n  Goodbye!\n\n");
            break;
        }
        else if (strcmp(cmd, "sync") == 0)
        {
            sync();
        }
        else if (strcmp(cmd, "list") == 0)
        {
            showPeers();
        }
        else if (strcmp(cmd, "help") == 0)
        {
            showHelp();
        }
        else if (strcmp(cmd, "download") == 0)
        {
            char filename[256];
            printf("  Enter file name : ");
            fflush(stdout);
            if (!fgets(filename, sizeof(filename), stdin))
                continue;
            filename[strcspn(filename, "\n")] = 0;
            printf("\n");
            requestFileFromPeer(filename);
        }
        else if (strcmp(cmd, "upload") == 0)
        {
            char filepath[512];

            printf("  File path: ");
            fflush(stdout);
            if (!fgets(filepath, sizeof(filepath), stdin))
                continue;
            filepath[strcspn(filepath, "\n")] = 0;

            printf("\n");

           
            long long fileSize = getFileSize(filepath);
            if (fileSize < 0)
            {
                printf("    [Error] File not found or inaccessible: %s\n\n", filepath);
                continue;
            }

            printf("    [Info] File size: %lld bytes (%.2f MB)\n",
                   fileSize, fileSize / (1024.0 * 1024.0));

            
            char *bestPeerIP = NULL;
            int connectedPeerIdx = -1;
            findBestPeer(&bestPeerIP, &connectedPeerIdx);

           
            if (bestPeerIP == NULL || connectedPeerIdx < 0)
            {
                printf("    [Error] No available peer found.\n");
                printf("    Make sure other peers are online and have storage available.\n\n");
                continue;
            }

          
            char targetPeerMAC[32];
            strncpy(targetPeerMAC, peerList[connectedPeerIdx].mac, sizeof(targetPeerMAC) - 1);
            targetPeerMAC[sizeof(targetPeerMAC) - 1] = '\0';

            printf("    [Selected] Peer: %s (%s) with %lld MB available\n",
                   peerList[connectedPeerIdx].name,
                   bestPeerIP,
                   peerList[connectedPeerIdx].avail / (1024 * 1024));
            Peer temp;
            memset(&temp, 0, sizeof(temp));

           
            temp = peerList[connectedPeerIdx]; 

            if (strcmp(bestPeerIP, myIP) == 0)
            {
                printf("    [Info] Best peer is yourself. File is already local.\n");

                char *filename = strrchr(filepath, '/');
                if (!filename)
                    filename = strrchr(filepath, '\\');
                filename = filename ? filename + 1 : (char *)filepath;
                int myIdx = -1;
                for (int i = 0; i < peerCount; i++)
                {
                    if (strcmp(peerList[i].mac, myMAC) == 0)
                    {
                        myIdx = i;
                        break;
                    }
                }

                if (myIdx < 0)
                {
                    printf("    [Error] Cannot find self in peer list\n");
                    continue;
                }

               
                char currentTime[64];
                getCurrentTime(currentTime, sizeof(currentTime));

                strcpy(peerList[myIdx].timeStr, currentTime);
                strcpy(temp.timeStr, currentTime);
                peerList[myIdx].avail -= fileSize;
                temp.avail -= fileSize;
                SLEEP(1);
                addFileToPeer(&peerList[myIdx], filename);
                addFileToPeer(&temp, filename);
                printf("    [Info] Broadcasting\n");
                broadcastUpdateLine(currentTime,
                                    temp.mac,
                                    temp.ip,
                                    temp.name,
                                    temp.total,
                                    temp.avail,
                                    temp.fileCount,
                                    temp.files,
                                    DISCOVERY_PORT);
                printf("    [Info] Broadcasted\n");
                writeMyInfo(currentTime,
                            peerList[myIdx].mac,
                            peerList[myIdx].ip,
                            peerList[myIdx].name,
                            peerList[myIdx].total,
                            peerList[myIdx].avail);
                printf("    [OK] written\n");

                strcpy(peerList[myIdx].timeStr, currentTime);
                peerList[myIdx].avail = temp.avail;

                sortPeers();
                savePeers("peers.txt");

                printf("    [INFO] saved\n");

                printf("    [Success] Metadata updated.\n\n");
                continue;
            }


            printf("    [Uploading] Sending to %s...\n", bestPeerIP);
            sendFile(filepath, bestPeerIP);
            char *filename = strrchr(filepath, '/');
            if (!filename)
                filename = strrchr(filepath, '\\');
            filename = filename ? filename + 1 : (char *)filepath;
            int targetIdx = -1;
            for (int i = 0; i < peerCount; i++)
            {
                if (strcmp(peerList[i].mac, targetPeerMAC) == 0)
                {
                    targetIdx = i;
                    break;
                }
            }

            if (targetIdx < 0)
            {
                printf("    [Warning] Cannot find target peer in list to update metadata\n");
                continue;
            }

            char currentTime[64];
            getCurrentTime(currentTime, sizeof(currentTime));

            strcpy(peerList[targetIdx].timeStr, currentTime);
            strcpy(temp.timeStr, currentTime);
            peerList[targetIdx].avail -= fileSize;
            temp.avail -= fileSize;

            SLEEP(1);
            addFileToPeer(&peerList[targetIdx], filename);
            addFileToPeer(&temp, filename);
            printf("    [INFO] Broadcasting\n");
            broadcastUpdateLine(currentTime,
                                temp.mac,
                                temp.ip,
                                temp.name,
                                temp.total,
                                temp.avail,
                                temp.fileCount,
                                temp.files,
                                DISCOVERY_PORT);
            printf("    [INFO] Broadcasted\n");

            
            strcpy(peerList[targetIdx].timeStr, currentTime);
            peerList[targetIdx].avail = temp.avail;

            sortPeers();
            savePeers("peers.txt");
           

            printf("    [INFO] saved\n");

            printf("    [Success] Upload complete and metadata updated.\n\n");
        }
        else
        {
            printf("  Unknown command. Type 'help' for available commands.\n\n");
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}