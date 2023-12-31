#include "reco.hpp"
#include "cuda_files.h"

#include <map>

#include <opencv2/opencv.hpp>

#include <nlohmann/json.hpp>
#include <enet/enet.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <chrono>
#include <thread>
#include <iomanip>
#include <queue>
#include <fstream>
#include <tuple>
#include <math.h>

#include <string>
#include <sstream>  

#include <numeric>

#include <errno.h>
#include <cerrno>

#define MAIN_PORT 50000

#define MESSAGE_ECHO 0
#define MESSAGE_REGISTER 1
#define MESSAGE_NEXT_SERVICE_IP 2
#define DATA_TRANSMISSION 3
#define CLIENT_REGISTRATION 4
#define SIFT_TO_MATCHING 5

#define BOUNDARY 3

// message_type definitions
#define MSG_ECHO 0
#define MSG_SERVICE_REGISTER 11
#define MSG_PRIMARY_SERVICE 12
#define MSG_DATA_TRANSMISSION 13
#define MSG_MATCHING_SIFT 14
#define MSG_SIFT_TO_ENCODING 15
#define MSG_CLIENT_FRAME_DETECT 2

#define PACKET_SIZE 60000
#define MAX_PACKET_SIZE 60000
#define RES_SIZE 512
//#define TRAIN
#define UDP

using namespace std;
using namespace cv;
using json = nlohmann::json;

using namespace std::this_thread; // sleep_for, sleep_until
using namespace std::chrono;      // nanoseconds, system_clock, seconds

struct sockaddr_in localAddr;
struct sockaddr_in remoteAddr;
struct sockaddr_in main_addr;
struct sockaddr_in next_service_addr;
struct sockaddr_in sift_rec_addr;
struct sockaddr_in sift_rec_remote_addr;
struct sockaddr_in matching_rec_addr;
struct sockaddr_in matching_addr;
struct sockaddr_in client_addr;
struct sockaddr_in sift_to_matching_addr;

socklen_t addrlen = sizeof(remoteAddr);
socklen_t mrd_len = sizeof(matching_rec_addr);
bool isClientAlive = false;

queue<frame_buffer> frames, offloadframes;
queue<resBuffer> results;
queue<matching_sift> sift_data_queue;
int recognizedMarkerID;

vector<char *> onlineImages;
vector<char *> onlineAnnotations;

SiftData reconstructed_data;
matchingSiftItem receivedSiftData;
bool isSiftReconstructed = false;

// declaring variables needed for distributed operation
string service;
int service_value;
queue<inter_service_buffer> inter_service_data;

string next_service;

json sift_buffer_details;
deque<sift_data_item> sift_items;
int sbd_max = 100;

json matching_buffer_details;
deque<matching_item> matching_items;
int mbd_max = 100;

// hard coding the maps for each service, nothing clever needed about this
std::map<string, int> service_map = {
    {"primary", 1},
    {"sift", 2},
    {"encoding", 3},
    {"lsh", 4},
    {"matching", 5}};

std::map<int, string> service_map_reverse = {
    {1, "primary"},
    {2, "sift"},
    {3, "encoding"},
    {4, "lsh"},
    {5, "matching"}};

std::map<string, string> registered_services;

// json services = {
//    {"primary", {"10.30.100.1", "50001"}},
//      {"sift", {"10.30.101.1", "50002"}},
//      {"encoding", {"10.30.102.1", "50003"}},
//      {"lsh", {"10.30.103.1", "50004"}},
//      {"matching", {"10.30.104.1", "50005"}}};

json services = {
   {"primary", {"10.38.151.146", "50001"}},
   {"sift", {"10.38.151.146", "51002"}},
   {"encoding", {"10.38.151.146", "50003"}},
   {"lsh", {"10.38.151.146", "50004"}},
   {"matching", {"10.38.151.146", "50005"}}};

json services_primary_knowledge;

json services_outline = {
    {"name_val", {{"primary", 1}, {"sift", 2}, {"encoding", 3}, {"lsh", 4}, {"matching", 5}}},
    {"val_name", {{"1", "primary"}, {"2", "sift"}, {"3", "encoding"}, {"4", "lsh"}, {"5", "matching"}}}};

char *ip_to_bytes(char *client_ip)
{
    unsigned short a, b, c, d;
    sscanf(client_ip, "%hu.%hu.%hu.%hu", &a, &b, &c, &d);

    char *ip_buffer = new char[16];
    memset(ip_buffer, 0, strlen(ip_buffer) + 1);

    charint ib_a;
    ib_a.i = (int)a;
    memcpy(ip_buffer, ib_a.b, 4);

    charint ib_b;
    ib_b.i = (int)b;
    memcpy(&(ip_buffer[4]), ib_b.b, 4);

    charint ib_c;
    ib_c.i = (int)c;
    memcpy(&(ip_buffer[8]), ib_c.b, 4);

    charint ib_d;
    ib_d.i = (int)d;
    memcpy(&(ip_buffer[12]), ib_d.b, 4);

    return ip_buffer;
}

char *bytes_to_ip(char *client_ip)
{
    char tmp[4];

    memcpy(tmp, &(client_ip[0]), 4);
    int ib_a = *(int *)tmp;

    memcpy(tmp, &(client_ip[4]), 4);
    int ib_b = *(int *)tmp;

    memcpy(tmp, &(client_ip[8]), 4);
    int ib_c = *(int *)tmp;

    memcpy(tmp, &(client_ip[12]), 4);
    int ib_d = *(int *)tmp;

    string final_ip_string = to_string(ib_a) + "." + to_string(ib_b) + "." + to_string(ib_c) + "." + to_string(ib_d);

    char *final_ip = new char[final_ip_string.length()];
    strcpy(final_ip, final_ip_string.c_str());

    return final_ip;
}

void registerService(int sock)
{
    charint register_id;
    register_id.i = service_value; // ID of service registering itself

    charint message_type;
    message_type.i = MSG_SERVICE_REGISTER; // message to primary to register service

    char registering[16];
    memcpy(&(registering[8]), message_type.b, 4);
    memcpy(&(registering[12]), register_id.b, 4);

    int udp_status = sendto(sock, registering, sizeof(registering), 0, (struct sockaddr *)&main_addr, sizeof(main_addr));
    if (udp_status == -1)
    {
        cout << "Error sending: " << strerror(errno) << endl;
    }
    print_log(service, "0", "0", "Service " + string(service) + " is attempting to register with the primary service");
}

void *ThreadUDPReceiverFunction(void *socket)
{
    print_log(service, "0", "0", "UDP receiver thread created");

    char tmp[4];
    char buffer[60 + PACKET_SIZE];
    int sock = *((int *)socket);

    char *sift_res_buffer;
    int sift_res_buffer_size;

    int curr_recv_packet_no;
    int prev_recv_packet_no = 0;
    int total_packets_no;

    int last_packet_no = 0;
    int last_frame_no = 0;
    bool valid = true;

    int packet_tally;
    int sift_data_count;

    json frame_packets;

    char *results_buffer;

    char *previous_client;

    json matching_ns = services["matching"];
    string matching_ip = matching_ns[0];
    int matching_recv_port = 51005;

    if (enet_initialize() != 0)
	{
		printf("An error occurred while initializing ENet.\n");
		exit(EXIT_FAILURE);
	}


    if (service != "primary")
    {
        registerService(sock); // when first called, try to register with primary service
    }

    while (1)
    {
        memset(buffer, 0, sizeof(buffer));
        recvfrom(sock, buffer, PACKET_SIZE, 0, (struct sockaddr *)&remoteAddr, &addrlen);

        char device_id[4];
        char client_id[4];
        char *device_ip = inet_ntoa(remoteAddr.sin_addr);
        int device_port = htons(remoteAddr.sin_port);

        // copy client frames into frames buffer if main service
        frame_buffer curr_frame;
        memcpy(client_id, buffer, 4);
        memcpy(device_id, buffer, 4);

        curr_frame.client_id = (char *)device_id;

        //char curr_client[4];
        //memcpy(&(curr_client[0]), device_id, 4);
        char *curr_client = (char*)device_id;

        memcpy(tmp, &(buffer[4]), 4);
        curr_frame.frame_no = *(int *)tmp;

        memcpy(tmp, &(buffer[8]), 4);
        curr_frame.data_type = *(int *)tmp;

        memcpy(tmp, &(buffer[12]), 4);
        curr_frame.buffer_size = *(int *)tmp;

        if (service == "primary")
        {
            if (curr_frame.data_type == MSG_ECHO)
            {
                // if an echo message from the client
                string device_ip_print = device_ip;
                print_log(service, string(curr_frame.client_id), "0", "Received an echo message from client with IP " + device_ip_print + " and port " + to_string(device_port));
                charint echoID;
                echoID.i = curr_frame.frame_no;
                char echo[4];
                memcpy(echo, echoID.b, 4);
                sendto(sock, echo, sizeof(echo), 0, (struct sockaddr *)&remoteAddr, addrlen);
                print_log(service, string(curr_frame.client_id), "0", "Sent an echo reply");

                int client_ip_strlen = strlen(device_ip);

                char client_registration[16 + client_ip_strlen];

                charint client_reg_frame_no;
                client_reg_frame_no.i = 0; // no frame
                memcpy(&(client_registration[0]), client_reg_frame_no.b, 4);

                charint client_reg_id;
                client_reg_id.i = CLIENT_REGISTRATION;
                memcpy(&(client_registration[4]), client_reg_id.b, 4);

                charint device_ip_len;
                device_ip_len.i = client_ip_strlen;
                memcpy(&(client_registration[12]), device_ip_len.b, 4);
                memcpy(&(client_registration[16]), device_ip, client_ip_strlen);

                continue;
            }
            else if (curr_frame.data_type == MSG_SERVICE_REGISTER)
            {
                // when a service comes online it tells primary and "registers"
                string service_id;

                // at position 12 of the frame there is the ID value of the service to be registered
                int service_val = curr_frame.buffer_size;
                json val_names = (services_outline["val_name"]);

                auto so_service = val_names.find(to_string(service_val));
                string service_to_register = *so_service;
                string service_ip = device_ip;
                services_primary_knowledge[service_to_register] = {device_ip, device_port};
                print_log(service, "0", "0", "Service " + service_to_register + " on IP " + device_ip + " has come online and registered with primary");

                registered_services.insert({service_to_register, device_ip});

                charint message_type;
                message_type.i = MSG_SERVICE_REGISTER; // message to primary to register service

                charint register_status;
                register_status.i = 1;

                char registering[16];
                memcpy(&(registering[8]), message_type.b, 4);
                memcpy(&(registering[12]), register_status.b, 4);

                sendto(sock, registering, sizeof(registering), 0, (struct sockaddr *)&remoteAddr, sizeof(remoteAddr));
                print_log(service, "0", "0", "Sending confirmation to service " + service_to_register + " that service is now registered with primary");
            }
            else if (curr_frame.data_type == MSG_CLIENT_FRAME_DETECT)
            {
                print_log(service, string(curr_frame.client_id), to_string(curr_frame.frame_no),
                          "Frame " + to_string(curr_frame.frame_no) + " received and has a filesize of " +
                              to_string(curr_frame.buffer_size) + " Bytes");

                // copy frame image data into buffer
                curr_frame.buffer = (char *)malloc(curr_frame.buffer_size);
                memset(curr_frame.buffer, 0, curr_frame.buffer_size);
                memcpy(curr_frame.buffer, &(buffer[16]), curr_frame.buffer_size);

                // copy client ip and port into buffer
                curr_frame.client_ip = device_ip;
                curr_frame.client_port = device_port;

                frames.push(curr_frame);
            }
        }
        else
        {
            if (curr_frame.data_type == MSG_SERVICE_REGISTER)
            {
                int primary_service_status = curr_frame.buffer_size;
                if (primary_service_status != 1)
                {
                    break;
                }

                print_log(service, "0", "0", "Received confirmation from primary that this service is now logged as online and active");
            }
            else if (curr_frame.data_type == MSG_DATA_TRANSMISSION)
            {
                // performing logic to check that received data is supposed to be sent on
                memcpy(tmp, &(buffer[36]), 4);
                int previous_service_val = *(int *)tmp;
                if (previous_service_val == service_value - 1)
                {
                    // if the data received is from previous service, proceed
                    // with copying out the data
                    char tmp_ip[16];
                    memcpy(tmp_ip, &(buffer[16]), 16);
                    curr_frame.client_ip = (char *)tmp_ip;
                    
                    memcpy(tmp, &(buffer[32]), 4);
                    curr_frame.client_port = *(int *)tmp;

                    // only copy if not sift
                    if (service_value != 2) {
                        // copy sift details out
                        char sift_tmp_ip[16];
                        memcpy(sift_tmp_ip, &(buffer[40]), 16);
                        curr_frame.sift_ip = (char *)sift_tmp_ip;
                        cout << "[DEBUG] SIFT IP " << curr_frame.sift_ip << endl;

                        memcpy(tmp, &(buffer[56]), 4);
                        curr_frame.sift_port = *(int *)tmp;
                    }

                    // curr_frame.buffer = new char[curr_frame.buffer_size];
                    curr_frame.buffer = (char *)malloc(curr_frame.buffer_size);
                    memset(curr_frame.buffer, 0, curr_frame.buffer_size);
                    memcpy(curr_frame.buffer, &(buffer[60]), curr_frame.buffer_size);

                    frames.push(curr_frame);

                    string client_id_string = curr_frame.client_id;
                    string client_id_corr = client_id_string.substr(0, 4);
                    char *client_id_ptr = &client_id_corr[0];

                    // print_log(service, string(client_id_ptr), to_string(curr_frame.frame_no), "Received data from '" + services_outline["val_name"][to_string(previous_service_val)] + "' service and will now proceed with analysis");

                    // if matching service, proceed to request the corresponding data from sift
                    if (service_value == 5)
                    {
                        // copy sift details out
                        char sift_tmp_ip2[16];
                        memcpy(sift_tmp_ip2, &(buffer[40]), 16);
                        curr_frame.sift_ip = (char *)sift_tmp_ip2;

                        char* sift_ip = curr_frame.sift_ip; 
                        string sift_ip_string = sift_ip;  

                        json sift_ns = services["sift"];
                        string sift_port_string = sift_ns[1];
                        int sift_port = stoi(sift_port_string);

                        char ms_buffer[12];
                        memset(ms_buffer, 0, sizeof(ms_buffer));
                        memcpy(ms_buffer, curr_frame.client_id, 4);
                        
                        string client_id_string = curr_frame.client_id;
                        string client_id_corr = client_id_string.substr(0, 4);
                        curr_frame.client_id = &client_id_corr[0];

                        //memcpy(&(ms_buffer[0]), client_id, sizeof(client_id));

                        charint matching_sift_fno;
                        matching_sift_fno.i = curr_frame.frame_no;
                        memcpy(&(ms_buffer[4]), matching_sift_fno.b, 4);

                        charint matching_sift_id;
                        matching_sift_id.i = MSG_MATCHING_SIFT;
                        memcpy(&(ms_buffer[8]), matching_sift_id.b, 4);

                        inet_pton(AF_INET, sift_ip_string.c_str(), &(sift_rec_addr.sin_addr));
                        sift_rec_addr.sin_port = htons(sift_port);

                        int udp_status = sendto(sock, ms_buffer, sizeof(ms_buffer), 0, (struct sockaddr *)&sift_rec_addr, sizeof(sift_rec_addr));
                        
                        print_log(service, string(curr_frame.client_id), to_string(curr_frame.frame_no), "Requested data from sift service with details " +  sift_ip_string + ":" + to_string(sift_port) + ", request packet has size " + to_string(udp_status));

                        // free(ms_buffer);
                    }
                }

            }
            else if (curr_frame.data_type == CLIENT_REGISTRATION)
            {
                memcpy(tmp, &(buffer[8]), 4);
                int client_port = *(int *)tmp;

                memcpy(tmp, &(buffer[12]), 4);
                int client_ip_len = *(int *)tmp;

                char client_ip_tmp[client_ip_len];
                memcpy(client_ip_tmp, &(buffer[16]), client_ip_len + 1);

                // creating client object to return data to
                inet_pton(AF_INET, client_ip_tmp, &(client_addr.sin_addr));
                client_addr.sin_port = htons(client_port);
                print_log(service, string(curr_frame.client_id), to_string(curr_frame.frame_no),
                          "Received client registration details from main of IP '" + string(client_ip_tmp) + "' and port " + to_string(client_port));
            }
            else if (curr_frame.data_type == MSG_MATCHING_SIFT)
            {
                // a request sent from matching to retrieve sift data
                print_log(service, string(curr_frame.client_id), to_string(curr_frame.frame_no), "Received request from 'matching' for sift data for Frame " + to_string(curr_frame.frame_no) + " and client " + string(curr_frame.client_id));

                // find where in the JSON array is the matching frame
                string frame_to_find = string(curr_frame.client_id) + "_" + to_string(curr_frame.frame_no);
                int sbd_val = 0;
                int sbd_loc;
                for (auto sbd_it : sift_buffer_details)
                {
                    // "it" is of type json::reference and has no key() member
                    if (sbd_it == frame_to_find)
                    {
                        sbd_loc = sbd_val;
                    }
                    sbd_val++;
                }

                // copy out the sift data from the deque
                sift_data_item msd = sift_items[sbd_loc];
                char *msd_client_id = msd.client_id;
                int msd_frame_no = msd.frame_no.i;
                float msd_data_size = (float)msd.sift_data_size.i;
                char *msd_data_buffer = msd.sift_data;

                ENetHost *client;
                client = enet_host_create(NULL, 1, 2, 0, 0);

                ENetAddress address;
                ENetEvent event;
                ENetPeer *peer;

                // attempt connection to matching
                enet_address_set_host(&address, matching_ip.c_str());
                address.port = matching_recv_port;
                
                peer = enet_host_connect(client, &address, 2, 0);
                enet_peer_timeout(peer, 15, 5, 20);

                if (peer == NULL)
                {
                    print_log(service, string(curr_frame.client_id), to_string(curr_frame.frame_no), "No available peers for initiating an ENet connection to matching");
                    // system("pause");
                    exit(EXIT_FAILURE);
                }
                if (enet_host_service(client, &event, 1) > 0 && event.type == ENET_EVENT_TYPE_CONNECT)
                {
                    print_log(service, string(curr_frame.client_id), to_string(curr_frame.frame_no), "Connection to matching succeeded");
                }
                else
                {
                    enet_peer_reset(peer);
                    print_log(service, string(curr_frame.client_id), to_string(curr_frame.frame_no), "Connection to matching failed");
                }

                // preparing the buffer of the packets to be sent
                char to_m_buffer[20 + int(msd_data_size)];
                memset(to_m_buffer, 0, msd_data_size+20);
                memcpy(to_m_buffer, curr_frame.client_id, 4);

                charint curr_frame_no;
                curr_frame_no.i = msd_frame_no;
                memcpy(&(to_m_buffer[4]), curr_frame_no.b, 4);

                charint total_size;
                total_size.i = msd_data_size;
                memcpy(&(to_m_buffer[16]), total_size.b, 4);

                memcpy(&(to_m_buffer[20]), msd_data_buffer, msd_data_size);

                ENetPacket *packet = enet_packet_create(to_m_buffer, msd_data_size+20, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
                enet_peer_send(peer, 0, packet);

                print_log(service, string(curr_frame.client_id), to_string(curr_frame.frame_no), "Sent sift data to matching of size " + to_string(int(msd_data_size)));
        
        		enet_host_flush(client);
                // enet_peer_reset(peer);
                enet_peer_disconnect_now(peer, 0);
                enet_packet_destroy(event.packet);
                enet_host_destroy(client);

                // while (enet_host_service (client, & event, 10) > 0)
                // {
                //     switch (event.type)
                //     {
                //     case ENET_EVENT_TYPE_RECEIVE:
                //         enet_packet_destroy (event.packet);
                //         break;
                //     case ENET_EVENT_TYPE_DISCONNECT:
                //         print_log(service, string(curr_frame.client_id), to_string(curr_frame.frame_no), "Disconnection from matching succesful");
                //         break;
                //     }
                // }

                // free(msd_data_buffer);
            }
        
        }
    }
}

void *ThreadENETReceiver(void *socket) {
    print_log(service, "0", "0", "Created thread to listen for data packets");

    ENetAddress address;
    ENetHost * server;

    int listener_port = 51002;
    int listener_timeout = 100000000;

    address.host = ENET_HOST_ANY;
    address.port = listener_port;

    char sift_sending_ip[80];
    int sift_sending_port;

    int recv_data_len;
    char* recv_buffer;

    char tmp[4];

    char client_id[4];
    char *client_id_ptr = client_id;
    string client_id_string;
    string client_id_corr;

    int frame_no;
    int complete_data_size;

    char *sift_data_buffer;

    inter_service_buffer results;

    server = enet_host_create(&address, 100, 2, 0, 0);
    if (server == NULL)
    {
        fprintf(stderr, 
                "An error occurred while trying to create an ENet server host.\n");
        exit(EXIT_FAILURE);
    } else {
        print_log(service, "0", "0", "ENet server host created to listen on port " + to_string(listener_port));
    }

    ENetEvent event;
    print_log(service, "0", "0", "ENet server will keep listening for " + to_string(listener_timeout / 1000) + " seconds");
    while (enet_host_service(server, &event, listener_timeout) > 0)
    {
        
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            enet_address_get_host_ip(&event.peer->address, sift_sending_ip, sizeof(sift_sending_ip));
            sift_sending_port = event.peer -> address.port;

            print_log(service, "0", "0", "connection ");
            // event.peer -> data = (void *)"Client information";
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            recv_data_len = event.packet -> dataLength;
            recv_buffer = new char[recv_data_len];

            memset(recv_buffer, 0, sizeof(recv_data_len));
            memcpy(recv_buffer, event.packet -> data, recv_data_len);
            
            memcpy(client_id, recv_buffer, 4);
            client_id_string = client_id;
            client_id_corr = client_id_string.substr(0, 4);
            client_id_ptr = &client_id_corr[0];

            memcpy(tmp, &(recv_buffer[4]), 4);
            frame_no = *(int *)tmp;

            memcpy(tmp, &(recv_buffer[16]), 4);
            complete_data_size = *(int *)tmp;

            print_log(service, string(client_id_ptr), to_string(frame_no),
                    "data received for Frame " + to_string(0));

            // sift_data_buffer = new char[complete_data_size];
            // memset(sift_data_buffer, 0, sizeof(recv_data_len));
            // memcpy(sift_data_buffer, &(recv_buffer[20]), complete_data_size);

            // matching_sift curr_sift_data;
            // curr_sift_data.sift_data = sift_data_buffer;
            // curr_sift_data.frame_no = frame_no;
            // curr_sift_data.client_id = client_id_ptr;

            // sift_data_queue.push(curr_sift_data);

            enet_packet_destroy(event.packet); // clean up packet
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            print_log(service, "0", "0",
                    "sift has disconnected");
            /* Reset the peer's client information. */
            event.peer -> data = NULL;
        }
    }
    print_log(service, "0", "0", "ENet server has timed out");
}


void siftdata_reconstructor(char *sd_char_array, matchingSiftItem receivedSiftData)
{
    char tmp[4];
    int curr_posn = 0;

    memcpy(tmp, &(sd_char_array[curr_posn]), 4);
    int sd_num_pts = *(int *)tmp;
    reconstructed_data.numPts = sd_num_pts;
    curr_posn += 4;

    memcpy(tmp, &(sd_char_array[curr_posn]), 4);
    reconstructed_data.maxPts = *(int *)tmp;
    curr_posn += 4;

    SiftPoint *cpu_data = (SiftPoint *)calloc(sd_num_pts, sizeof(SiftPoint));
    // SiftPoint *cpu_data = (SiftPoint *)malloc(sd_num_pts, sizeof(SiftPoint));

    for (int i = 0; i < sd_num_pts; i++)
    {
        SiftPoint *curr_data = (&cpu_data[i]);

        memcpy(tmp, &(sd_char_array[curr_posn]), 4);
        curr_data->xpos = *(float *)tmp;
        curr_posn += 4;

        memcpy(tmp, &(sd_char_array[curr_posn]), 4);
        curr_data->ypos = *(float *)tmp;
        curr_posn += 4;

        memcpy(tmp, &(sd_char_array[curr_posn]), 4);
        curr_data->scale = *(float *)tmp;
        curr_posn += 4;

        memcpy(tmp, &(sd_char_array[curr_posn]), 4);
        curr_data->sharpness = *(float *)tmp;
        curr_posn += 4;

        memcpy(tmp, &(sd_char_array[curr_posn]), 4);
        curr_data->edgeness = *(float *)tmp;
        curr_posn += 4;

        memcpy(tmp, &(sd_char_array[curr_posn]), 4);
        curr_data->orientation = *(float *)tmp;
        curr_posn += 4;

        memcpy(tmp, &(sd_char_array[curr_posn]), 4);
        curr_data->score = *(float *)tmp;
        curr_posn += 4;

        memcpy(tmp, &(sd_char_array[curr_posn]), 4);
        curr_data->ambiguity = *(float *)tmp;
        curr_posn += 4;

        memcpy(tmp, &(sd_char_array[curr_posn]), 4);
        curr_data->match = *(int *)tmp;
        curr_posn += 4;

        memcpy(tmp, &(sd_char_array[curr_posn]), 4);
        curr_data->match_xpos = *(float *)tmp;
        curr_posn += 4;

        memcpy(tmp, &(sd_char_array[curr_posn]), 4);
        curr_data->match_ypos = *(float *)tmp;
        curr_posn += 4;

        memcpy(tmp, &(sd_char_array[curr_posn]), 4);
        curr_data->match_error = *(float *)tmp;
        curr_posn += 4;

        memcpy(tmp, &(sd_char_array[curr_posn]), 4);
        curr_data->subsampling = *(float *)tmp;
        curr_posn += 4;

        // re-creating the empty array
        for (int j = 0; j < 3; j++)
        {
            memcpy(tmp, &(sd_char_array[curr_posn]), 4);
            curr_data->empty[j] = *(float *)tmp;
            curr_posn += 4;
        }

        for (int k = 0; k < 128; k++)
        {
            memcpy(tmp, &(sd_char_array[curr_posn]), 4);
            curr_data->data[k] = *(float *)tmp;
            curr_posn += 4;
        }
    }

    reconstructed_data.h_data = cpu_data; // inserting data into reconstructed data structure
    receivedSiftData.data = reconstructed_data;
    print_log(service, "0", "0", "SiftData has been reconstructed from sift service, example data: " + to_string(sd_num_pts) + " SIFT points");
}

void *matching_sift_data_analyser(void *input){
    while (1)
    {
        if (sift_data_queue.empty())
        {
            this_thread::sleep_for(chrono::milliseconds(1));
            continue;
        }

        matching_sift sift_data_buffer = sift_data_queue.front();
        sift_data_queue.pop();

        recognizedMarker marker;
        bool markerDetected = false;

        matching_item md;
        char *md_client_id;
        char *md_client_ip;
        int md_client_port;
        int md_frame_no;
        vector<int> result;

        int frame_no = sift_data_buffer.frame_no;
        char* client_id = sift_data_buffer.client_id;

        inter_service_buffer curRes;

        char *buffer;

        siftdata_reconstructor(sift_data_buffer.sift_data, receivedSiftData);

        receivedSiftData.frame_no = frame_no;

        // find where in the JSON array is the matching frame
        string frame_to_find = string(client_id) + "_" + to_string(frame_no);

        this_thread::sleep_for(chrono::milliseconds(5));

        int mbd_val = 0;
        int mbd_loc;
        for (auto mbd_it : matching_buffer_details)
        {
            if (mbd_it == frame_to_find)
            { 
                mbd_loc = mbd_val;
            }
            mbd_val++;
        }

        // copy out the matching data from the deque
        md = matching_items[mbd_loc];
        md_client_id = md.client_id;
        md_client_ip = md.client_ip;
        md_client_port = md.client_port;
        md_frame_no = md.frame_no;
        result = md.lsh_result;

        markerDetected = matching(result, reconstructed_data, marker);

        if (markerDetected)
        {
            curRes.client_id = md_client_id;
            curRes.frame_no.i = md_frame_no;
            curRes.data_type.i = MSG_DATA_TRANSMISSION;
            curRes.buffer_size.i = 1;
            curRes.client_ip = md_client_ip;
            curRes.client_port.i = md_client_port;
            curRes.previous_service.i = BOUNDARY;

            curRes.results_buffer = new char[100 * curRes.buffer_size.i];
            //curRes.buffer = (unsigned char*)malloc(100 * curRes.buffer_size.i);
            // memset(curRes.results_buffer, 0, 100 * curRes.buffer_size.i);
            
            int pointer = 0;
            memcpy(&(curRes.results_buffer[pointer]), marker.markerID.b, 4);
            pointer += 4;
            memcpy(&(curRes.results_buffer[pointer]), marker.height.b, 4);
            pointer += 4;
            memcpy(&(curRes.results_buffer[pointer]), marker.width.b, 4);
            pointer += 4;

            charfloat p;
            for (int j = 0; j < 4; j++)
            {
                p.f = marker.corners[j].x;
                memcpy(&(curRes.results_buffer[pointer]), p.b, 4);
                pointer += 4;
                p.f = marker.corners[j].y;
                memcpy(&(curRes.results_buffer[pointer]), p.b, 4);
                pointer += 4;
            }

            memcpy(&(curRes.results_buffer[pointer]), marker.markername.data(), marker.markername.length());

            recognizedMarkerID = marker.markerID.i;

            print_log(service, string(md_client_id), to_string(md_frame_no), "matching analysis is complete, will pass to client forwarder");

            // cout << recognizedMarkerID << endl;
        }
        else
        {
            curRes.frame_no.i = frame_no;
            curRes.buffer_size.i = 0;
        }

        inter_service_data.push(curRes);
    }
}

void *udp_sift_data_listener(void *socket) {
    print_log(service, "0", "0", "Created thread to listen for SIFT data packets for the matching service");

    ENetAddress address;
    ENetHost * server;

    int listener_port = 51005;
    int listener_timeout = 100000000;

    address.host = ENET_HOST_ANY;
    address.port = listener_port;

    char sift_sending_ip[80];
    int sift_sending_port;

    int recv_data_len;
    char* recv_buffer;

    char tmp[4];

    char client_id[4];
    char *client_id_ptr = client_id;
    string client_id_string;
    string client_id_corr;

    int frame_no;
    int complete_data_size;

    char *sift_data_buffer;

    inter_service_buffer results;

    server = enet_host_create(&address, 100, 2, 0, 0);
    if (server == NULL)
    {
        fprintf(stderr, 
                "An error occurred while trying to create an ENet server host.\n");
        exit(EXIT_FAILURE);
    } else {
        print_log(service, "0", "0", "ENet server host created to listen on port " + to_string(listener_port));
    }

    ENetEvent event;
    print_log(service, "0", "0", "ENet server will keep listening for " + to_string(listener_timeout / 1000) + " seconds");
    while (enet_host_service(server, &event, listener_timeout) > 0)
    {
        
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            enet_address_get_host_ip(&event.peer->address, sift_sending_ip, sizeof(sift_sending_ip));
            sift_sending_port = event.peer -> address.port;

            print_log(service, "0", "0", "sift has connected to matching, sift data will be received soon by matching from host " + string(sift_sending_ip) +  " and port " + to_string(sift_sending_port));
            // event.peer -> data = (void *)"Client information";
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            recv_data_len = event.packet -> dataLength;
            recv_buffer = new char[recv_data_len];

            memset(recv_buffer, 0, sizeof(recv_data_len));
            memcpy(recv_buffer, event.packet -> data, recv_data_len);
            
            memcpy(client_id, recv_buffer, 4);
            client_id_string = client_id;
            client_id_corr = client_id_string.substr(0, 4);
            client_id_ptr = &client_id_corr[0];

            memcpy(tmp, &(recv_buffer[4]), 4);
            frame_no = *(int *)tmp;

            memcpy(tmp, &(recv_buffer[16]), 4);
            complete_data_size = *(int *)tmp;

            print_log(service, string(client_id_ptr), to_string(frame_no),
                    "sift data received for Frame " + to_string(0) + " and will now attempt to reconstruct into a SiftData structure");

            sift_data_buffer = new char[complete_data_size];
            memset(sift_data_buffer, 0, sizeof(recv_data_len));
            memcpy(sift_data_buffer, &(recv_buffer[20]), complete_data_size);

            matching_sift curr_sift_data;
            curr_sift_data.sift_data = sift_data_buffer;
            curr_sift_data.frame_no = frame_no;
            curr_sift_data.client_id = client_id_ptr;

            sift_data_queue.push(curr_sift_data);

            enet_packet_destroy(event.packet); // clean up packet
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            print_log(service, "0", "0",
                    "sift has disconnected");
            /* Reset the peer's client information. */
            event.peer -> data = NULL;
        }
    }
    print_log(service, "0", "0", "ENet server has timed out, data from sift will not be listened for now - please restart the matching service");
}

void *udp_encoding_data_listener(void *socket) {
    print_log(service, "0", "0", "Created thread to listen for SIFT data packets for the encoding service");

    ENetAddress address;
    ENetHost *server;

    int listener_port = MAIN_PORT + service_value;
    int listener_timeout = 100000000;

    address.host = ENET_HOST_ANY;
    address.port = listener_port;

    char sift_sending_ip[80];
    int sift_sending_port;

    char tmp[4];
    int recv_data_len;
    char* recv_buffer;

    char device_id[4];
    char client_id[4];
    char *client_id_ptr = client_id;
    string client_id_string;
    string client_id_corr;

    char *curr_client;

    char tmp_ip[16];

    char *sift_res_buffer;
    int sift_res_buffer_size;

    server = enet_host_create(&address, 100, 2, 0, 0);
    if (server == NULL)
    {
        fprintf(stderr, 
                "An error occurred while trying to create an ENet server host.\n");
        exit(EXIT_FAILURE);
    } else {
        print_log(service, "0", "0", "ENet server host created to listen on port " + to_string(listener_port));
    }

    ENetEvent event;
    print_log(service, "0", "0", "ENet server will keep listening for " + to_string(listener_timeout / 1000) + " seconds");
    while (enet_host_service(server, &event, listener_timeout) > 0)
    {
        frame_buffer curr_frame;

        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            enet_address_get_host_ip(&event.peer->address, sift_sending_ip, sizeof(sift_sending_ip));
            sift_sending_port = event.peer -> address.port;

            print_log(service, "0", "0", "sift has connected to encoding, sift data will be received soon by encoding from host " + string(sift_sending_ip) +  " and port " + to_string(sift_sending_port));
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            recv_data_len = event.packet -> dataLength;
            recv_buffer = new char[recv_data_len];

            memset(recv_buffer, 0, recv_data_len);
            memcpy(recv_buffer, event.packet -> data, recv_data_len);

            memcpy(client_id, recv_buffer, 4);
            client_id_string = client_id;
            client_id_corr = client_id_string.substr(0, 4);
            client_id_ptr = &client_id_corr[0];

            memcpy(device_id, recv_buffer, 4);

            curr_frame.client_id = (char *)device_id;
            curr_client = (char*)device_id;

            memcpy(tmp, &(recv_buffer[4]), 4);
            curr_frame.frame_no = *(int *)tmp;

            memcpy(tmp, &(recv_buffer[8]), 4);
            curr_frame.data_type = *(int *)tmp;

            memcpy(tmp, &(recv_buffer[12]), 4);
            curr_frame.buffer_size = *(int *)tmp;
            
            if (curr_frame.data_type == MSG_SIFT_TO_ENCODING)
            {
                memcpy(tmp_ip, &(recv_buffer[16]), 16);
                curr_frame.client_ip = (char *)tmp_ip;

                memcpy(tmp, &(recv_buffer[32]), 4);
                curr_frame.client_port = *(int *)tmp;

                // store the sift IP and port details 
                curr_frame.sift_ip = sift_sending_ip;
                curr_frame.sift_port = sift_sending_port;

                memcpy(tmp, &(recv_buffer[12]), 4);
                sift_res_buffer_size = *(int *)tmp;

                sift_res_buffer = new char[sift_res_buffer_size];
                memset(sift_res_buffer, 0, sift_res_buffer_size);
                memcpy(sift_res_buffer, &(recv_buffer[48]), sift_res_buffer_size);

                curr_frame.buffer = (char *)malloc(curr_frame.buffer_size);
                memset(curr_frame.buffer, 0, curr_frame.buffer_size);
                memcpy(curr_frame.buffer, sift_res_buffer, curr_frame.buffer_size);

                frames.push(curr_frame);

                print_log(service, string(client_id_ptr), to_string(curr_frame.frame_no),
                    "sift data received for Frame " + to_string(0) + " and will now attempt to perform encoding");
            }

            enet_packet_destroy(event.packet); // clean up packet
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            print_log(service, "0", "0", "sift has disconnected");
            /* Reset the peer's client information. */
            event.peer -> data = NULL;
        }
    }
    print_log(service, "0", "0", "ENet server has timed out, data from sift will not be listened for now - please restart the matching service");
}

void *ThreadUDPSenderFunction(void *socket)
{
    print_log(service, "0", "0", "UDP sender thread created");

    char buffer[RES_SIZE];
    int sock = *((int *)socket);

    socklen_t next_service_addrlen = sizeof(next_service_addr);

    if (service_value < 5)
    {
        next_service = service_map_reverse.at(service_value + 1);
    }

    while (1)
    {
        if (inter_service_data.empty())
        {
            this_thread::sleep_for(chrono::milliseconds(1));
            continue;
        }
        
        // generate new socket everytime data is needed to be sent
        int next_service_socket;
        struct sockaddr_in next_service_sock;

        if (service != "sift") {
            if ((next_service_socket = ::socket(AF_INET, SOCK_DGRAM, 0)) < 0)
            {
                perror("socket creation failed");
                exit(EXIT_FAILURE);
            }
            memset((char*)&next_service_sock, 0, sizeof(next_service_sock));

            // Filling server information
            next_service_sock.sin_family = AF_INET;
            next_service_sock.sin_port = htons(0);
            next_service_sock.sin_addr.s_addr = INADDR_ANY;

            // Forcefully attaching socket to the port 8080
            if (bind(next_service_socket, (struct sockaddr*)&next_service_sock,
                    sizeof(next_service_sock))
                < 0) {
                print_log(service, "0", "0", "ERROR: Unable to bind UDP");
                exit(1);
            }
        }
        
        if (service == "primary")
        {
            inter_service_buffer curr_item = inter_service_data.front();
            inter_service_data.pop();

            char buffer[60 + curr_item.buffer_size.i];
            memset(buffer, 0, sizeof(buffer));

            memcpy(buffer, curr_item.client_id, 4);
            memcpy(&(buffer[4]), curr_item.frame_no.b, 4);
            memcpy(&(buffer[8]), curr_item.data_type.b, 4);
            memcpy(&(buffer[12]), curr_item.buffer_size.b, 4);
            memcpy(&(buffer[16]), curr_item.client_ip, 16);
            memcpy(&(buffer[32]), curr_item.client_port.b, 4);
            memcpy(&(buffer[36]), curr_item.previous_service.b, 4);
            memcpy(&(buffer[60]), &(curr_item.image_buffer)[0], curr_item.buffer_size.i + 1);
            // sendto(next_service_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&next_service_addr, next_service_addrlen);
            // close(next_service_socket);
            print_log(service, string(curr_item.client_id), to_string(curr_item.frame_no.i),
                      "Frame " + to_string(curr_item.frame_no.i) + " sent to " + next_service +
                          " service for processing with a payload size of " + to_string(curr_item.buffer_size.i));

            ENetHost * client;
            client = enet_host_create(NULL, 1, 2, 0, 0);

            ENetAddress address;
            ENetEvent event;
            ENetPeer *peer;

            int next_service_val = service_value + 1;
            string next_service = service_map_reverse[next_service_val];
            json next_service_details = services[next_service];

            string next_service_ip = next_service_details[0];
            string next_service_port_string = next_service_details[1];
            int next_service_port = stoi(next_service_port_string);

            cout << next_service_ip << " " << next_service_port << endl;

            enet_address_set_host(&address, next_service_ip.c_str());
            address.port = next_service_port;

            peer = enet_host_connect(client, &address, 2, 0);
            enet_peer_timeout(peer, 15, 5, 20);

            if (peer == NULL)
            {
                print_log(service, string(curr_item.client_id), to_string(curr_item.frame_no.i), "No available peers for initiating an ENet connection to matching");
                // system("pause");
                exit(EXIT_FAILURE);
            }
            if (enet_host_service(client, &event, 1) > 0 && event.type == ENET_EVENT_TYPE_CONNECT)
            {
                print_log(service, string(curr_item.client_id), to_string(curr_item.frame_no.i), "Connection to encoding succeeded");
            }
            else
            {
                enet_peer_reset(peer);
                print_log(service, string(curr_item.client_id), to_string(curr_item.frame_no.i), "Connection to encoding failed");
            }

            ENetPacket *packet = enet_packet_create(buffer, sizeof(buffer), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
            enet_peer_send(peer, 0, packet);

            print_log(service, string(curr_item.client_id), to_string(curr_item.frame_no.i), "Sent data of size ");

            enet_host_flush(client);
            // enet_peer_reset(peer);
            enet_peer_disconnect_now(peer, 0);
            enet_packet_destroy(event.packet);
            enet_host_destroy(client);
        }
        else if (service == "sift")
        {
            // client_addr
            inter_service_buffer curr_item = inter_service_data.front();
            inter_service_data.pop();

            float total_packet_size = (float)curr_item.buffer_size.i;

            char sift_buffer[48 + int(total_packet_size)];
            memset(sift_buffer, 0, 48 + total_packet_size);

            memcpy(sift_buffer, curr_item.client_id, 4);
            memcpy(&(sift_buffer[4]), curr_item.frame_no.b, 4);
            memcpy(&(sift_buffer[8]), curr_item.data_type.b, 4);
            memcpy(&(sift_buffer[12]), curr_item.buffer_size.b, 4);
            memcpy(&(sift_buffer[16]), curr_item.client_ip, 16);
            memcpy(&(sift_buffer[32]), curr_item.client_port.b, 4);
            memcpy(&(sift_buffer[36]), curr_item.previous_service.b, 4);

            memcpy(&(sift_buffer[48]), curr_item.buffer, int(total_packet_size));

            ENetHost * client;
            client = enet_host_create(NULL, 1, 2, 0, 0);

            ENetAddress address;
            ENetEvent event;
            ENetPeer *peer;

            int next_service_val = service_value + 1;
            string next_service = service_map_reverse[next_service_val];
            json next_service_details = services[next_service];

            string next_service_ip = next_service_details[0];
            string next_service_port_string = next_service_details[1];
            int next_service_port = stoi(next_service_port_string);

            enet_address_set_host(&address, next_service_ip.c_str());
            address.port = next_service_port;

            peer = enet_host_connect(client, &address, 2, 0);
            enet_peer_timeout(peer, 15, 5, 20);

            if (peer == NULL)
            {
                print_log(service, string(curr_item.client_id), to_string(curr_item.frame_no.i), "No available peers for initiating an ENet connection to matching");
                // system("pause");
                exit(EXIT_FAILURE);
            }
            if (enet_host_service(client, &event, 1) > 0 && event.type == ENET_EVENT_TYPE_CONNECT)
            {
                print_log(service, string(curr_item.client_id), to_string(curr_item.frame_no.i), "Connection to encoding succeeded");
            }
            else
            {
                enet_peer_reset(peer);
                print_log(service, string(curr_item.client_id), to_string(curr_item.frame_no.i), "Connection to encoding failed");
            }

            ENetPacket *packet = enet_packet_create(sift_buffer, total_packet_size+48, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
            enet_peer_send(peer, 0, packet);

            print_log(service, string(curr_item.client_id), to_string(curr_item.frame_no.i), "Sent sift data to lsh of size " + to_string(total_packet_size));

            enet_host_flush(client);
            // enet_peer_reset(peer);
            enet_peer_disconnect_now(peer, 0);
            enet_packet_destroy(event.packet);
            enet_host_destroy(client);

            // free(curr_item.buffer);
        }
        else if (service == "matching")
        {
            // client_addr
            inter_service_buffer curr_res = inter_service_data.front();
            inter_service_data.pop();

            char tmp[4];

            int buffer_size = curr_res.buffer_size.i;

            char buffer[16 + (100*buffer_size)];
            memset(buffer, 0, sizeof(buffer));

            memcpy(buffer, curr_res.client_id, 4);
            memcpy(&(buffer[4]), curr_res.frame_no.b, 4);
            memcpy(&(buffer[12]), curr_res.buffer_size.b, 4);
            if (buffer_size != 0)
                memcpy(&(buffer[16]), curr_res.results_buffer, 100 * buffer_size);

            memcpy(tmp, curr_res.frame_no.b, 4);
            int frame_no = *(int*)tmp;

            char *client_return_ip = curr_res.client_ip;
            string client_return_ip_string = client_return_ip;

            memcpy(tmp, curr_res.client_port.b, 4);
            int client_return_port = *(int*)tmp;

            inet_pton(AF_INET, client_return_ip, &(client_addr.sin_addr));
            client_addr.sin_port = htons(client_return_port);

            int udp_status = sendto(next_service_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, sizeof(client_addr));
            print_log(service, string(curr_res.client_id), to_string(frame_no), "Results for Frame " + to_string(frame_no) +
                          " sent to client with number of markers of " + to_string(buffer_size));
                          
            close(next_service_socket);
            if (udp_status == -1)
            {
                printf("Error sending: %i\n", errno);
            }
            // char *client_device_ip = inet_ntoa(client_addr.sin_addr);
            // //string client_device_ip_string = client_device_ip;
            // int client_device_port = htons(client_addr.sin_port);

            // cout << "[DEBUG] client has IP of " << client_device_ip << " and port " << to_string(client_device_port) << endl;
            }
        else
        {
            inter_service_buffer curr_item = inter_service_data.front();
            inter_service_data.pop();

            int item_data_size = curr_item.buffer_size.i;
            
            char buffer[60 + item_data_size];
            memset(buffer, 0, sizeof(buffer));

            memcpy(buffer, curr_item.client_id, 4);
            memcpy(&(buffer[4]), curr_item.frame_no.b, 4);
            memcpy(&(buffer[8]), curr_item.data_type.b, 4);
            memcpy(&(buffer[12]), curr_item.buffer_size.b, 4);
            memcpy(&(buffer[16]), curr_item.client_ip, 16);
            memcpy(&(buffer[32]), curr_item.client_port.b, 4);
            memcpy(&(buffer[36]), curr_item.previous_service.b, 4);
            memcpy(&(buffer[40]), curr_item.sift_ip, 16);
            memcpy(&(buffer[56]), curr_item.sift_port.b, 4);
            memcpy(&(buffer[60]), &(curr_item.buffer)[0], curr_item.buffer_size.i);

            int udp_status = sendto(next_service_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&next_service_addr, next_service_addrlen);
            close(next_service_socket);
            if (udp_status == -1)
            {
                printf("Error sending: %i\n", errno);
            }

            print_log(service, string(curr_item.client_id), to_string(curr_item.frame_no.i),
                      "Forwarded Frame " + to_string(curr_item.frame_no.i) + " for client " +
                          string(curr_item.client_id) + " to '" + next_service + "' service for processing" +
                          " with a payload size of " + to_string(curr_item.buffer_size.i));
            // free(curr_item.buffer);
        }
    }
}

void *ThreadProcessFunction(void *param)
{
    print_log(service, "0", "0", "Processing thread created");

    inter_service_buffer item;
    char tmp[4];

    while (1)
    {
        if (frames.empty())
        {
            this_thread::sleep_for(chrono::milliseconds(1));
            continue;
        }

        frame_buffer curr_frame = frames.front();
        frames.pop();

        char *client_id = curr_frame.client_id;
        int frame_no = curr_frame.frame_no;
        int frame_data_type = curr_frame.data_type;
        int frame_size = curr_frame.buffer_size;

        char *client_ip = curr_frame.client_ip;
        int client_port = curr_frame.client_port;
        char *frame_data = curr_frame.buffer;

        if (frame_data_type == MSG_CLIENT_FRAME_DETECT)
        {
            if (service == "primary")
            {
                print_log(service, string(client_id), to_string(frame_no),
                          "Image from Frame has been reduced from size " + to_string(frame_size) +
                              " to a Mat object of size " + to_string(frame_size));

                item.client_id = client_id;
                item.frame_no.i = frame_no;
                item.data_type.i = MSG_DATA_TRANSMISSION;
                item.buffer_size.i = frame_size;
                item.client_ip = client_ip;
                item.client_port.i = client_port;
                item.previous_service.i = service_value;
                item.image_buffer = frame_data;

                inter_service_data.push(item);
            }
        }
        else if (frame_data_type == MSG_DATA_TRANSMISSION || frame_data_type == MSG_SIFT_TO_ENCODING)
        {
            if (service == "sift")
            {
                SiftData tData;
                float sift_array[2];
                int sift_result;
                float sift_resg;
                int height, width;
                vector<float> test;

                int sift_points;        // number of SIFT points
                char *sift_data_buffer; // raw data of the SIFT analysis
                char *raw_sift_data;    // raw SIFT data needed by matching

                vector<uchar> imgdata(frame_data, frame_data + frame_size);
                Mat img_scene = imdecode(imgdata, IMREAD_GRAYSCALE);
                imwrite("query.jpg", img_scene);
                Mat detect = img_scene(Rect(RECO_W_OFFSET, RECO_H_OFFSET, 160, 270));

                sift_processing(sift_points, &sift_data_buffer, &raw_sift_data, detect, tData);

                char tmp[4];
                memcpy(tmp, &(raw_sift_data[0]), 4);
                int sd_num_pts = *(int *)tmp;

                charint siftresult;
                siftresult.i = sift_points;
                int sift_buffer_size = 128 * 4 * siftresult.i; // size of char values

                // push data required for next service
                item.client_id = client_id;
                item.frame_no.i = frame_no;
                item.data_type.i = MSG_SIFT_TO_ENCODING;
                item.buffer_size.i = 4 + sift_buffer_size;
                item.client_ip = client_ip;
                item.client_port.i = client_port;
                item.previous_service.i = service_value;

                // item.buffer = new unsigned char[4 + sift_buffer_size];
                item.buffer = (unsigned char *)malloc(4 + sift_buffer_size);
                // memset(item.buffer, 0, 4 + sift_buffer_size);
                memset(item.buffer, 0, 4 + sift_buffer_size);
                memcpy(&(item.buffer[0]), siftresult.b, 4);
                memcpy(&(item.buffer[4]), sift_data_buffer, sift_buffer_size);

                inter_service_data.push(item);

                // storing SIFT data for retrieval by the matching service
                int sift_data_size = 4 * siftresult.i * (15 + 3 + 128); // taken from export_siftdata
                print_log(service, string(client_id), to_string(frame_no),
                          "Expected size of SIFT data buffer to store for Frame " + to_string(frame_no) +
                              " is " + to_string(sift_data_size) + " Bytes");

                // create a buffer to store for matching to retrieve
                sift_data_item curr_sdi;
                curr_sdi.client_id = item.client_id;
                curr_sdi.frame_no.i = item.frame_no.i;
                curr_sdi.sift_data_size.i = sift_data_size;
                curr_sdi.sift_data = raw_sift_data;

                int sbd_count = sift_items.size();
                if (sbd_count == sbd_max)
                {
                    // sift_data_item item_to_delete = sift_items.front();
                    // char *itd_sift_buffer = item_to_delete.sift_data;
                    // free(itd_sift_buffer);
                    sift_items.pop_front(); // pop front item if 10 items
                }
                sift_items.push_back(curr_sdi); // append to end of the 10 items
                deque<sift_data_item>::iterator it;
                for (it = sift_items.begin(); it != sift_items.end(); ++it)
                {
                    sift_data_item curr_item = *it;
                }
                if (int(sift_buffer_details.size()) == sbd_max)
                {
                    sift_buffer_details.erase(0);
                }
                sift_buffer_details.push_back(string(item.client_id) + "_" + to_string(item.frame_no.i));

                print_log(service, string(client_id), to_string(frame_no),
                          "Storing SIFT data for client " + string(item.client_id) + " and Frame " +
                              to_string(frame_no) + " in SIFT data buffer for collection by matching");

                FreeSiftData(tData);
                free(frame_data);
                free(sift_data_buffer);
                // free(raw_sift_data);
            }
            else if (service == "encoding")
            {
                vector<float> test;

                memcpy(tmp, &(frame_data[0]), 4);
                int sift_result = *(int *)tmp;

                char *sift_resg = (char *)malloc(frame_size);
                memset(sift_resg, 0, frame_size);
                memcpy(sift_resg, &(frame_data[4]), frame_size);

                float *siftres = new float[128 * sift_result];
                // float *siftres = (float*)malloc(128*sift_result);

                int data_index = 0;
                for (int i = 0; i < sift_result * 128; i++)
                {
                    memcpy(tmp, &(sift_resg[data_index]), 4);
                    float curr_float = *(float *)tmp;
                    siftres[i] = curr_float;

                    data_index += 4;
                }

                char *encoded_vec = new char[4 * 5248];
                auto encoding_results = encoding(siftres, sift_result, test, false, &encoded_vec);

                charint encoded_size;
                encoded_size.i = get<0>(encoding_results);
                int encoding_buffer_size = 4 * encoded_size.i; // size of char values

                char *encoded_vector = get<1>(encoding_results);

                string client_id_string = client_id;
                string client_id_corr = client_id_string.substr(0, 4);
                char *client_id_ptr = &client_id_corr[0];

                item.client_id = client_id_ptr;
                item.frame_no.i = frame_no;
                item.data_type.i = MSG_DATA_TRANSMISSION;
                item.buffer_size.i = 4 + encoding_buffer_size;
                item.client_ip = client_ip;
                item.client_port.i = client_port;
                item.previous_service.i = service_value;
                item.buffer = new unsigned char[4 + encoding_buffer_size];
                memset(item.buffer, 0, strlen((char*)item.buffer)+1);
                memcpy(&(item.buffer[0]), encoded_size.b, 4);
                memcpy(&(item.buffer[4]), encoded_vector, encoding_buffer_size);

                item.sift_ip = curr_frame.sift_ip;
                item.sift_port.i = curr_frame.sift_port;

                inter_service_data.push(item);
                print_log(service, string(client_id_ptr), to_string(frame_no), "Performed encoding on received 'sift' data");

                delete[] siftres;
                delete[] encoded_vec;

                free(frame_data);
                free(sift_resg);
            }
            else if (service == "lsh")
            {
                vector<float> enc_vec;

                memcpy(tmp, &(frame_data[0]), 4);
                int enc_size = *(int *)tmp;

                char *enc_vec_char = (char *)malloc(enc_size * 4);
                memcpy(enc_vec_char, &(frame_data[4]), 4 * enc_size);

                // looping through char array to convert data back into floats
                // at i = 0, index should begin at 4
                int data_index = 0;
                for (int i = 0; i < enc_size; i++)
                {
                    memcpy(tmp, &(enc_vec_char[data_index]), 4);
                    float *curr_float = (float *)tmp;
                    enc_vec.push_back(*curr_float);

                    data_index += 4;
                }
                auto results_returned = lsh_nn(enc_vec);

                charint results_size;
                results_size.i = get<0>(results_returned);
                int results_buffer_size = 4 * results_size.i; // size of char values

                char *results_vector = get<1>(results_returned);

                item.client_id = client_id;
                item.frame_no.i = frame_no;
                item.data_type.i = MSG_DATA_TRANSMISSION;
                item.buffer_size.i = 4 + results_buffer_size;
                item.client_ip = client_ip;
                item.client_port.i = client_port;
                item.previous_service.i = service_value;

                item.buffer = (unsigned char *)malloc(4 + results_buffer_size);
                memset(item.buffer, 0, 4 + results_buffer_size);
                memcpy(&(item.buffer[0]), results_size.b, 4);
                memcpy(&(item.buffer[4]), results_vector, results_buffer_size);

                char* sift_ip = curr_frame.sift_ip;
                int sift_port = curr_frame.sift_port;

                item.sift_ip = sift_ip;
                item.sift_port.i = sift_port;

                inter_service_data.push(item);
                print_log(service, string(client_id), to_string(frame_no), "Performed analysis on received 'encoding' data");
            }
            else if (service == "matching")
            {
                vector<int> result;

                memcpy(tmp, &(frame_data[0]), 4);
                int result_size = *(int *)tmp;
                    
                char *results_char = new char[result_size * 4];
                memset(results_char, 0, result_size*4);
                memcpy(results_char, &(frame_data[4]), result_size*4);

                int data_index = 0;
                for (int i = 0; i < result_size; i++)
                {
                    memcpy(tmp, &(results_char[data_index]), 4);
                    int *curr_int = (int *)tmp;
                    result.push_back(*curr_int);

                    data_index += 4;
                }

                string client_id_string = client_id;
                string client_id_corr = client_id_string.substr(0, 4);
                char *client_id_ptr = &client_id_corr[0];

                // create buffer to store for when sift data is reconstructed
                matching_item curr_mi;
                curr_mi.client_id = client_id_ptr;
                curr_mi.client_ip = client_ip;
                curr_mi.client_port = client_port;
                curr_mi.frame_no = frame_no;
                curr_mi.lsh_result = result;

                int mi_count = matching_items.size();
                if ((mi_count-mbd_max)==0)
                {
                    matching_items.pop_front();
                }
                matching_items.push_back(curr_mi); // append to end of the 10 items
                deque<matching_item>::iterator it;
                for (it = matching_items.begin(); it != matching_items.end(); ++it)
                {
                    matching_item curr_item = *it;
                }
                if (int(matching_buffer_details.size()) == mbd_max)
                {
                    matching_buffer_details.erase(0);
                }
                matching_buffer_details.push_back(string(client_id_ptr) + "_" + to_string(frame_no));
                print_log(service, string(client_id_ptr), to_string(frame_no), "Storing data from lsh for Frame " + to_string(frame_no));
                //delete[] results_char;
            }
        }
    }
}

void runServer(int port, string service)
{
    pthread_t senderThread, receiverThread, imageProcessThread, processThread;
    pthread_t sift_listen_thread, encoding_listen_thread, matching_sift_thread, enet_listener_thread;
    char buffer[PACKET_SIZE];
    char fileid[4];
    int status = 0;
    int sockUDP;
    int sl_udp_sock;

    memset((char *)&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    localAddr.sin_port = htons(port);

    if ((sockUDP = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        print_log(service, "0", "0", "ERROR: Unable to open UDP socket");
        exit(1);
    }
    if (bind(sockUDP, (struct sockaddr *)&localAddr, sizeof(localAddr)) < 0)
    {
        print_log(service, "0", "0", "ERROR: Unable to bind UDP");
        exit(1);
    }
    print_log(service, "0", "0", "Server UDP port for service " + service + " is bound to " + to_string(port));

    isClientAlive = true;
    pthread_create(&receiverThread, NULL, ThreadUDPReceiverFunction, (void *)&sockUDP);
    pthread_create(&senderThread, NULL, ThreadUDPSenderFunction, (void *)&sockUDP);
    pthread_create(&imageProcessThread, NULL, ThreadProcessFunction, NULL);

    // if primary service, set the details of the next service
    if (service == "primary" || service == "matching")
    {
        json sift_ns = services["sift"];
        string sift_ip = sift_ns[0];
        string sift_port_string = sift_ns[1];
        int sift_port = stoi(sift_port_string);
        if (service == "primary")
        {
            //inet_pton(AF_INET, sift_ip.c_str(), &(next_service_addr.sin_addr));
            next_service_addr.sin_family = AF_INET;
            next_service_addr.sin_addr.s_addr = inet_addr(sift_ip.c_str());
            next_service_addr.sin_port = htons(sift_port);
        }
        else if (service == "matching")
        {
            pthread_create(&sift_listen_thread, NULL, udp_sift_data_listener, NULL);
            pthread_create(&matching_sift_thread, NULL, matching_sift_data_analyser, NULL);
        }
    } 
    else if(service == "sift")
    {
        cout << "AHHH" << endl;
        pthread_create(&enet_listener_thread, NULL, ThreadENETReceiver, NULL);
    }
    else if (service == "encoding")
    {
        pthread_create(&encoding_listen_thread, NULL, udp_encoding_data_listener, NULL);
    }

    // check if there is a following service, and attempt to contact
    json val_names = (services_outline["val_name"]);
    int next_service_val = service_value + 1;
    auto nsv = val_names.find(to_string(next_service_val));
    if ((nsv != val_names.end()) == true)
    {
        // if there is a following service, set the details of it
        string next_service = *nsv;
        json next_service_details = services[next_service];

        string next_service_ip = next_service_details[0];
        string next_service_port_string = next_service_details[1];
        string next_ip_corr = next_service_port_string.substr(0, 5);
        int next_service_port = stoi(next_ip_corr);
        
        inet_pton(AF_INET, next_service_ip.c_str(), &(next_service_addr.sin_addr));
        print_log(service, "0", "0", "Setting the details of the next service '" + next_service + "' to have an IP of " + next_service_ip + " and port " + to_string(next_service_port));
        next_service_addr.sin_port = htons(next_service_port);
    }

    pthread_join(receiverThread, NULL);
    pthread_join(senderThread, NULL);
    pthread_join(imageProcessThread, NULL);

    if (service == "sift")
    {
        pthread_join(enet_listener_thread, NULL);
    }

    if (service == "matching")
    {
        pthread_join(sift_listen_thread, NULL);
        pthread_join(matching_sift_thread, NULL);
    }
}

void loadOnline()
{
    ifstream file("data/onlineData.dat");
    string line;
    int i = 0;
    while (getline(file, line))
    {
        char *fileName = new char[256];
        strcpy(fileName, line.c_str());

        if (i % 2 == 0)
            onlineImages.push_back(fileName);
        else
            onlineAnnotations.push_back(fileName);
        ++i;
    }
    file.close();
}

inline string getCurrentDateTime(string s)
{
    time_t now = time(0);
    struct tm tstruct;
    char buf[80];
    tstruct = *localtime(&now);
    if (s == "now")
        strftime(buf, sizeof(buf), "%Y-%m-%d %X", &tstruct);
    else if (s == "date")
        strftime(buf, sizeof(buf), "%Y-%m-%d", &tstruct);
    return string(buf);
}

int main(int argc, char *argv[])
{
    int querysizefactor, nn_num;

    // current service name and value in the service map
    service = string(argv[1]);
    service_value = service_map.at(argv[1]);

    print_log(service, "0", "0", "Selected service is: " + string(argv[1]));
    print_log(service, "0", "0", "IP of the primary module provided is " + string(argv[2]));

    int pp_req[4]{2, 3, 4, 5}; // pre-processing required

    if (find(begin(pp_req), end(pp_req), service_value) != end(pp_req))
    {
        // performing initial variable loading and encoding
        loadOnline();
        loadImages(onlineImages);
        loadParams();

        // arbitrarily encoding the above variables
        querysizefactor = 3;
        nn_num = 5;
        encodeDatabase(querysizefactor, nn_num);
    }

    // setting the specified host IP address and the hardcoded port
    inet_pton(AF_INET, argv[2], &(main_addr.sin_addr));
    main_addr.sin_port = htons(50000 + int(service_map.at("primary")));

    int port = MAIN_PORT + service_value; // hardcoding the initial port

    if (service == "encoding") {
        port = port + 100;
    }

    runServer(port, service);

    freeParams();
    return 0;
}