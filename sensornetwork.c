#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <mpi.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/socket.h>
#include "temp_queue.h"

#define SHIFT_ROW 0
#define SHIFT_COL 1
#define DISP 1
#define TEMP_THRESHOLD 90
#define TEMP_DIFF_THRESHOLD 5
#define ADJACENT_NODES 4
#define NUM_THREADS 2
#define QUEUEMAX 30
#define TIME_SLEEP 1
#define TIME_LIMIT 15

// MPI tag definitions
#define MAC_ADDRESS_MSG 93
#define IP_ADDRESS_MSG 94
#define ALIVE_MSG 95
#define TERMINATE 96
#define CONFIRM_TERMINATE 97
#define ALERT_TAG 98
#define TEMP_REQUEST 99

struct satellite_params
{
    int rank;
    int m;
    int n;
    FILE *file;
};

struct fault_params
{
    MPI_Comm comm;
    int ncols;
};

typedef struct
{
    int year;
    int month;
    int day;
    int hour;
    int min;
    float sec;
} event_date_time;

struct Event_message
{
    event_date_time time;
    int temp;
    int adj_node_count;
    adj_node adj_node[ADJACENT_NODES];
};

int random_temp(int rank, int min, int max);
int check_for_request(MPI_Comm master_comm, MPI_Comm comm, int temperature, int rank, FILE *f);
void request_adj_node_temp(MPI_Comm comm, int *adj_node_rank, int *adj_node_temp, MPI_Request *send_req, MPI_Request *recv_req, int rank, FILE *f);
int coord_to_rank(tuple coord, int ncols);
void *infaredSatellite(void *pArg);
tuple rank_to_coord(int rank, int ncols);
int read_from_file(char *file_name);
void log_record(int alert_type, int found, int iterNum, struct Temp_entry satellite_node, struct Event_message alert, int alert_rank, int ncols, int *mp_count_arr, double *avg_time, int n, unsigned char (*mac_arr)[18]);
void *fault_detection(void *pArg);
void base_station(int size, int nrows, int ncols, MPI_Datatype data_type, MPI_Comm comm);
void sensor_node(int argc, int size, int rank, int nrows, int ncols, MPI_Datatype data_type, MPI_Comm comm, MPI_Comm master_comm);
void log_summary(int *mp_total_counter, int *mp_true_counter, int *mp_sat_false_counter, int *mp_false_counter, double avg_time, int n);
int getSum(int *arr, int n);
int getMac(unsigned char *mac);

int satelliteTemp = 0;
int n_nodes;
int fault = 0;
pthread_mutex_t lock;
struct infraredQueue *satelliteQ;

int main(int argc, char *argv[])
{
    satelliteQ = buildQueue(30);
    int size, rank, nrows, ncols;
    MPI_Comm new_comm;

    struct Event_message buf;
    MPI_Datatype PackedMes;
    MPI_Datatype type[4] = {MPI_BYTE, MPI_INT, MPI_INT, MPI_INT};
    int blocklen[4] = {6, 1, 1, 4};
    MPI_Aint disp[4];

    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_split(MPI_COMM_WORLD, rank == size - 1, 0, &new_comm);

    MPI_Status sensor_status[size - 1];
    MPI_Request sensor_req[size - 1];

    n_nodes = size - 1;

    int *start_add = &buf;
    int *time_add = &buf.time;
    int *temp_add = &buf.temp;
    int *count_add = &buf.adj_node_count;
    int *coord_add = &buf.adj_node;

    disp[0] = time_add - start_add;
    disp[1] = temp_add - start_add;
    disp[2] = count_add - start_add;
    disp[3] = coord_add - start_add;
    MPI_Type_create_struct(4, blocklen, disp, type, &PackedMes);
    MPI_Type_commit(&PackedMes);

    nrows = atoi(argv[1]);
    ncols = atoi(argv[2]);

    if (rank == size - 1) // base station
    {
        base_station(size, nrows, ncols, PackedMes, MPI_COMM_WORLD);
    }
    else // sensor nodes
    {
        sensor_node(argc, size, rank, nrows, ncols, PackedMes, new_comm, MPI_COMM_WORLD);
    }
    fflush(stdout);

    MPI_Finalize();

    return 0;
}

int random_temp(int rank, int min, int max)
{
    /*
        Generates a random temperature value [min, max]
    */
    int temp;

    srand(time(NULL) + rank * 89);
    temp = rand() % (max + 1 - min) + min;

    return temp;
}

int check_for_request(MPI_Comm master_comm, MPI_Comm comm, int temperature, int rank, FILE *f)
{
    /*
        Checks if there's any request to send its temperature value.
    */
    int flag = 0;
    int receiver; // variable with no meaningful value to act as a buffer for MPI_Recv
    int req_rank; // rank that sent the temperature request
    MPI_Status status;

    fprintf(f, "rank %d is probing\n", rank);
    MPI_Iprobe(MPI_ANY_SOURCE, TERMINATE, master_comm, &flag, &status);
    if (flag)
    {
        fprintf(f, "rank %d received a termination message\n", rank);
        return -1;
    }

    MPI_Iprobe(MPI_ANY_SOURCE, TEMP_REQUEST, comm, &flag, &status);
    if (flag)
    {
        req_rank = status.MPI_SOURCE;
        fprintf(f, "Rank %d received a request from Rank %d\n", rank, req_rank);
        MPI_Recv(&receiver, 0, MPI_INT, req_rank, TEMP_REQUEST, comm, &status);
        MPI_Send(&temperature, 1, MPI_INT, req_rank, 0, comm);
        fprintf(f, "Rank %d sent %d to rank %d\n", rank, temperature, req_rank);
    }
    return 0;
}

void request_adj_node_temp(MPI_Comm comm, int *adj_node_rank, int *adj_node_temp, MPI_Request *send_req, MPI_Request *recv_req, int rank, FILE *f)
{
    /*
        Sends a temperature request to each adjacent node.

        The rank parameter is for debugging purposes, remove it before submission.
    */
    int sender; // variable with no meaningful value to act as a buffer for MPI_Isend

    for (int i = 0; i < ADJACENT_NODES; i++)
    {
        if (adj_node_rank[i] >= 0)
        {
            MPI_Isend(&sender, 0, MPI_INT, adj_node_rank[i], TEMP_REQUEST, comm, &send_req[i]);
            fprintf(f, "rank %d sent a request to %d\n", rank, adj_node_rank[i]);
            MPI_Irecv(&adj_node_temp[i], 1, MPI_INT, adj_node_rank[i], 0, comm, &recv_req[i]);
        }
        else
        {
            send_req[i] = recv_req[i] = MPI_REQUEST_NULL;
        }
    }
}

void *infaredSatellite(void *pArg)
{
    struct Temp_entry entry;
    struct satellite_params *params = pArg;

    int rank = *(&params->rank);
    int mNodes = *(&params->m);
    int nNodes = *(&params->n);
    FILE *f = *(&params->file);

    while (n_nodes)
    {
        satelliteTemp = random_temp(rank, 65, 95);

        tuple randCoord;
        int x = rand() % (mNodes);
        int y = rand() % (nNodes);
        tuple t = {x, y};

        entry.time = time(NULL);
        entry.coord = t;
        entry.temp = satelliteTemp;
        pthread_mutex_lock(&lock);
        enqueue(satelliteQ, entry, f);
        pthread_mutex_unlock(&lock);
        sleep(TIME_SLEEP);
    }
}

void *fault_detection(void *pArg)
{

    struct fault_params *params = pArg;

    MPI_Comm comm = *(&params->comm);
    int ncols = *(&params->ncols);

    struct timespec clock_time;
    int flag;
    MPI_Status status;
    int buf, elapsed_time;
    FILE *f;
    time_t c_time = time(NULL);
    struct tm log_time = *localtime(&c_time);

    clock_gettime(CLOCK_MONOTONIC, &clock_time);

    pthread_mutex_lock(&lock);
    struct timespec *node_last_timestamp = (struct timespec *)malloc(n_nodes * sizeof(struct timespec));
    struct tm *node_time_log = (struct tm *)malloc(n_nodes * sizeof(struct tm));
    for (int i = 0; i < n_nodes; i++)
    {
        node_last_timestamp[i].tv_sec = clock_time.tv_sec;
        node_time_log[i] = log_time;
    }

    pthread_mutex_unlock(&lock);

    int local_n_node = n_nodes;
    while (n_nodes)
    {
        for (int i = 0; i < local_n_node; i++)
        {
            MPI_Iprobe(i, ALIVE_MSG, comm, &flag, &status);
            if (flag == 1)
            {
                MPI_Recv(&buf, 0, MPI_INT, status.MPI_SOURCE, ALIVE_MSG, comm, &status);

                clock_gettime(CLOCK_MONOTONIC, &clock_time);
                log_time = *localtime(&c_time);

                node_last_timestamp[status.MPI_SOURCE].tv_sec = clock_time.tv_sec;
                node_time_log[status.MPI_SOURCE] = log_time;
                flag = 0;
            }
        }

        for (int i = 0; i < local_n_node; i++)
        {
            clock_gettime(CLOCK_MONOTONIC, &clock_time);
            c_time = time(NULL);
            log_time = *localtime(&c_time);

            elapsed_time = (clock_time.tv_sec - node_last_timestamp[i].tv_sec);

            if (elapsed_time >= TIME_LIMIT)
            {
                tuple fault_coord = rank_to_coord(i, ncols);
                f = fopen("base_station_fault_log.txt", "w");
                fprintf(f, "###################################\n");
                fprintf(f, "FAULT DETECTED!\nExceeded time limit of %d seconds\n\n", TIME_LIMIT);
                fprintf(f, "Faulty node: %d, ", i);
                fprintf(f, "Coord: (%d, %d)\n", fault_coord.x, fault_coord.y);
                fprintf(f, "Log time: %d-%02d-%02d %02d:%02d:%02d\n", log_time.tm_year + 1900, log_time.tm_mon + 1, log_time.tm_mday, log_time.tm_hour, log_time.tm_min, log_time.tm_sec - 1);
                fprintf(f, "Last logged time: %d-%02d-%02d %02d:%02d:%02d\n", node_time_log[i].tm_year + 1900, node_time_log[i].tm_mon + 1, node_time_log[i].tm_mday, node_time_log[i].tm_hour, node_time_log[i].tm_min, node_time_log[i].tm_sec);
                fprintf(f, "Elapsed time: %d seconds\n", elapsed_time);
                fprintf(f, "###################################\n");
                fclose(f);
                fault = 1;
                break;
            }
        }
        if (fault)
            break;
        sleep(1);
    }
}

void base_station(int size, int nrows, int ncols, MPI_Datatype data_type, MPI_Comm comm)
{
    int buf;
    long n, i;
    int rank = size - 1;
    int flag;
    int threadNum[2];
    pthread_t tid[2];
    MPI_Status status;
    MPI_Status send_status[ADJACENT_NODES]; // not used but crashed if removed
    MPI_Request send_req[ADJACENT_NODES];
    MPI_Request recv_req[ADJACENT_NODES];
    MPI_Request mac_req[size - 1];
    MPI_Status sensor_status[size - 1];
    MPI_Request sensor_req[size - 1];

    FILE *bs_event_log;

    bs_event_log = fopen("base_station_event_log.txt", "w");

    int mp_total_counter[size - 1]; // array of total message passed for each sensor node
    int mp_true_counter[size - 1];
    int mp_sat_false_counter[size - 1];
    int mp_false_counter[size - 1];
    unsigned char mac_arr[size - 1][18];
    double avg_time = 0;

    pthread_mutex_init(&lock, NULL);
    struct satellite_params params1 = {rank, nrows, ncols, bs_event_log};
    pthread_create(&tid[0], 0, infaredSatellite, &params1);

    // thread for fault detection
    struct fault_params params2 = {comm, ncols};
    pthread_create(&tid[1], 0, fault_detection, &params2);

    // initialise counters
    for (int i = 0; i < size - 1; i++)
    {
        mp_total_counter[i] = mp_true_counter[i] = mp_sat_false_counter[i] = mp_false_counter[i] = 0;
    }

    // run_base_station
    char mac_buf[17];
    for (int i = 0; i < size - 1; i++)
    {
        MPI_Recv(&mac_buf, 17, MPI_CHAR, i, MAC_ADDRESS_MSG, comm, MPI_STATUS_IGNORE);
        strcpy(mac_arr[i], mac_buf);
    }

    i = 0;
    n = read_from_file("user_input.txt");

    while (i < n && !fault)
    {
        // printf("%ld\n", i);
        fprintf(bs_event_log, "\nIteration %ld\n", i);
        // ===== for base station =====
        flag = 0;
        MPI_Iprobe(MPI_ANY_SOURCE, ALERT_TAG, comm, &flag, &status);

        if (flag)
        {
            struct Event_message alert;
            MPI_Recv(&alert, 4, data_type, MPI_ANY_SOURCE, ALERT_TAG, comm, &status);
            int alert_rank = status.MPI_SOURCE;

            // increment msg passing counter
            mp_total_counter[alert_rank]++;

            fprintf(bs_event_log, "Base station received alert from rank %d\n", alert_rank);

            pthread_mutex_lock(&lock);
            int alert_type = 0;
            int found = 0; // indication if coord is found in array
            for (int m = satelliteQ->size - 1; m >= 0; m--)
            {
                if (coord_to_rank(satelliteQ->array[m].coord, ncols) == alert_rank)
                {
                    found = 1;
                    if (abs(satelliteQ->array[m].temp - alert.temp) <= TEMP_DIFF_THRESHOLD)
                    {
                        mp_true_counter[alert_rank]++;
                        log_record(alert_type, found, i, satelliteQ->array[m], alert, alert_rank, ncols, mp_total_counter, &avg_time, size - 1, mac_arr);
                    }
                    else
                    {
                        alert_type = 1;
                        mp_sat_false_counter[alert_rank]++;
                        log_record(alert_type, found, i, satelliteQ->array[m], alert, alert_rank, ncols, mp_total_counter, &avg_time, size - 1, mac_arr);
                    }
                    break;
                }
            }
            pthread_mutex_unlock(&lock);
            if (found == 0)
            {
                struct Temp_entry empty_entry;
                alert_type = 2;
                mp_false_counter[alert_rank]++;
                log_record(alert_type, found, i, empty_entry, alert, alert_rank, ncols, mp_total_counter, &avg_time, size - 1, mac_arr);
            }
        }
        n = read_from_file("user_input.txt"); // update n value if user changes its value in the text file
        i++;
        sleep(TIME_SLEEP);
    }

    log_summary(mp_total_counter, mp_true_counter, mp_sat_false_counter, mp_false_counter, avg_time, size - 1);

    fprintf(bs_event_log, "Signalling sensor nodes to terminate\n");
    for (int i = 0; i < size - 1; i++)
    {
        MPI_Isend(&buf, 0, MPI_INT, i, TERMINATE, comm, &sensor_req[i]);
    }

    MPI_Waitall(size - 1, sensor_req, sensor_status);
    fprintf(bs_event_log, "All sensor nodes have been been signalled to terminate\n");
    while (n_nodes > 0)
    {
        MPI_Recv(&buf, 0, MPI_INT, MPI_ANY_SOURCE, CONFIRM_TERMINATE, comm, &status);
        n_nodes--;
    }

    for (i = 0; i < NUM_THREADS; i++)
    {
        pthread_join(tid[i], NULL);
    }

    // pthread_join(tid[0], NULL);

    fprintf(bs_event_log, "Infrared satellite has terminated\n");
    fclose(bs_event_log);
    pthread_mutex_destroy(&lock);
}

void sensor_node(int argc, int size, int rank, int nrows, int ncols, MPI_Datatype data_type, MPI_Comm comm, MPI_Comm master_comm)
{
    long i;
    int ndims = 2, reorder, ierr, cart_rank;
    int dims[ndims], coord[ndims], wrap_around[ndims];
    int local_temp, buf;
    MPI_Comm comm2d;
    int nbr_i_lo, nbr_i_hi, nbr_j_lo, nbr_j_hi;
    int nbr_j_lo_temp, nbr_j_hi_temp, nbr_i_lo_temp, nbr_i_hi_temp;
    int send_flag, recv_flag;
    unsigned char mac[17];
    char *ip;

    MPI_Status status;
    MPI_Status send_status[ADJACENT_NODES]; // not used but crashed if removed
    MPI_Request send_req[ADJACENT_NODES];
    MPI_Request recv_req[ADJACENT_NODES];
    MPI_Request mac_req;
    MPI_Request ip_req;

    FILE *f;

    getMac(mac);

    MPI_Send(&mac, 17, MPI_CHAR, size - 1, MAC_ADDRESS_MSG, master_comm);

    // from FIT3143 lab week 9 task 1

    if (argc == 3)
    {
        dims[0] = nrows;
        dims[1] = ncols;
        if ((nrows * ncols) != size - 1)
        {
            if (rank == 0)
                printf("ERROR: nrows*ncols) = %d * %d = %d != %d\n", nrows, ncols, nrows * ncols, size - 1);
            return;
        }
    }
    else
    {
        nrows = ncols = (int)sqrt(size - 1);
        dims[0] = dims[1] = 0;
    }

    MPI_Dims_create(size - 1, ndims, dims);

    /* create cartesian mapping */
    wrap_around[0] = wrap_around[1] = 0;
    reorder = 1;
    ierr = 0;
    ierr = MPI_Cart_create(comm, ndims, dims, wrap_around, reorder, &comm2d);
    if (ierr != 0)
        printf("ERROR[%d] creating CART\n", ierr);

    /* find my coordinates in the cartesian communicator group */
    MPI_Cart_coords(comm2d, rank, ndims, coord);
    /* use my cartesian coordinates to find my rank in cartesian group*/
    MPI_Cart_rank(comm2d, coord, &cart_rank);
    /* get my neighbors; axis is coordinate dimension of shift */
    MPI_Cart_shift(comm2d, SHIFT_ROW, DISP, &nbr_i_lo, &nbr_i_hi); // identify top and bottom ranks
    MPI_Cart_shift(comm2d, SHIFT_COL, DISP, &nbr_j_lo, &nbr_j_hi); // identify left and right ranks

    int adjNodesRank[ADJACENT_NODES] = {nbr_j_lo, nbr_j_hi, nbr_i_lo, nbr_i_hi};
    int adjNodesTemp[ADJACENT_NODES] = {nbr_j_lo_temp, nbr_j_hi_temp, nbr_i_lo_temp, nbr_i_hi_temp};

    f = fopen("sensor_event_log.txt", "a");

    int ret_code;
    i = 0;
    while (1)
    {
        // too simulate a fault
        // if (rank == 0 && i == 5)
        // {
        //     break;
        // }

        local_temp = random_temp(rank, 65, 90);

        ret_code = check_for_request(master_comm, comm2d, local_temp, rank, f);
        if (ret_code == -1)
        {
            break;
        }

        if (local_temp >= TEMP_THRESHOLD)
        {
            fprintf(f, "Rank: %d exceeding threshold with temp: %d\n", rank, local_temp);
            request_adj_node_temp(comm2d, adjNodesRank, adjNodesTemp, send_req, recv_req, rank, f);

            // initial check if all the send and receive requests are completed
            MPI_Testall(ADJACENT_NODES, send_req, &send_flag, MPI_STATUS_IGNORE);
            MPI_Testall(ADJACENT_NODES, recv_req, &recv_flag, MPI_STATUS_IGNORE);

            while (!(send_flag && recv_flag))
            {
                fprintf(f, "Rank %d is waiting\n", rank);
                if (!send_flag)
                {
                    MPI_Testall(ADJACENT_NODES, send_req, &send_flag, MPI_STATUS_IGNORE);
                }
                if (!recv_flag)
                {
                    MPI_Testall(ADJACENT_NODES, recv_req, &recv_flag, MPI_STATUS_IGNORE);
                }
                ret_code = check_for_request(master_comm, comm2d, local_temp, rank, f);
                if (ret_code == -1)
                {
                    break;
                }
                sleep(TIME_SLEEP);
            }
            if (ret_code == -1)
            {
                break;
            }
            int count, adj_count;
            count = adj_count = 0;
            struct Event_message mes;
            for (int i = 0; i < ADJACENT_NODES; i++)
            {
                if (adjNodesRank[i] >= 0)
                {
                    if (abs(local_temp - adjNodesTemp[i]) <= TEMP_DIFF_THRESHOLD)
                    {
                        count++;
                    }
                    adj_node sn = {adjNodesRank[i], adjNodesTemp[i]};
                    mes.adj_node[adj_count] = sn;
                    adj_count++;
                }
            }
            if (count >= 2)
            {
                struct timespec ts;
                struct tm t_m;
                time_t t = time(NULL);
                t_m = *localtime(&t);
                clock_gettime(CLOCK_REALTIME, &ts);
                event_date_time d_t = {t_m.tm_year + 1900, t_m.tm_mon + 1, t_m.tm_mday, t_m.tm_hour, t_m.tm_min, t_m.tm_sec + ts.tv_nsec * 1e-9};
                mes.time = d_t;
                mes.temp = local_temp;
                mes.adj_node_count = adj_count;
                tuple coord = rank_to_coord(rank, ncols);
                fprintf(f, "(Rank %d) coord (%d, %d) is sending an alert with temp %d\n", rank, coord.x, coord.y, mes.temp);
                MPI_Send(&mes, 4, data_type, size - 1, ALERT_TAG, master_comm);
            }
        }
        MPI_Send(&buf, 0, MPI_INT, size - 1, ALIVE_MSG, master_comm);
        i++;
        sleep(TIME_SLEEP);
    }
    fclose(f);
    MPI_Send(&buf, 0, MPI_INT, size - 1, CONFIRM_TERMINATE, master_comm);
}

int coord_to_rank(tuple coord, int ncols)
{
    return coord.x * ncols + coord.y;
}

tuple rank_to_coord(int rank, int ncols)
{
    int quotient = rank / ncols;
    int remainder = rank % ncols;
    tuple coord = {quotient, remainder};
    return coord;
}

int read_from_file(char *file_name)
{
    FILE *f = fopen(file_name, "r");
    int n;

    fscanf(f, "%d", &n);

    return n;
}

void log_record(int alert_type, int found, int iterNum, struct Temp_entry satellite_node, struct Event_message alert, int alert_rank, int ncols, int *mp_count_arr, double *avg_time, int n, unsigned char (*mac_arr)[18])
{
    FILE *bs_log;

    tuple alert_coord = rank_to_coord(alert_rank, ncols);
    tuple adj_coord;
    event_date_time alert_time = alert.time;
    int mp_count = mp_count_arr[alert_rank];
    int total_count = getSum(mp_count_arr, n);

    // write true alert log to file
    time_t c_time = time(NULL);
    struct tm log_time = *localtime(&c_time);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    float t_elapsed = (log_time.tm_sec + ts.tv_nsec * 1e-9) - alert_time.sec;
    if (t_elapsed < 0)
    {
        t_elapsed = t_elapsed + 60;
    }

    *avg_time = ((*avg_time * (total_count - 1)) + t_elapsed) / total_count;

    int overheat_node_count = 0;
    for (int n = 0; n < alert.adj_node_count; n++)
    {
        if (abs(alert.temp - alert.adj_node[n].temp) <= TEMP_DIFF_THRESHOLD)
        {
            overheat_node_count++;
        }
    }

    bs_log = fopen("base_station_log.txt", "a");

    fprintf(bs_log, "------------------------------------------------------\n");
    fprintf(bs_log, "Iteration: %d\n", iterNum);
    fprintf(bs_log, "Logged Time: %d-%02d-%02d %02d:%02d:%02d\n", log_time.tm_year + 1900, log_time.tm_mon + 1, log_time.tm_mday, log_time.tm_hour, log_time.tm_min, log_time.tm_sec);
    fprintf(bs_log, "Alert Reported Time: %d-%02d-%02d %02d:%02d:%02d\n", alert_time.year, alert_time.month, alert_time.day, alert_time.hour, alert_time.min, (int)alert_time.sec);
    if (alert_type == 0)
    {
        fprintf(bs_log, "Alert Type: True\n\n");
    }
    else if (alert_type == 1)
    {
        fprintf(bs_log, "Alert Type: False\n\n");
    }
    else
    {
        fprintf(bs_log, "Alert Type: False (Infrared satellite reading not found)\n\n");
    }
    fprintf(bs_log, "Reporting Node      Coord    Temp    MAC\n");
    fprintf(bs_log, "%-20d(%d, %d)   %-8d%-24s\n\n", alert_rank, alert_coord.x, alert_coord.y, alert.temp, mac_arr[alert_rank]);

    fprintf(bs_log, "Adjacent Nodes      Coord    Temp    MAC\n");
    for (int n = 0; n < alert.adj_node_count; n++)
    {
        int rank = alert.adj_node[n].rank;
        adj_coord = rank_to_coord(rank, ncols);
        fprintf(bs_log, "%-20d(%d, %d)   %-8d%-24s\n", rank, adj_coord.x, adj_coord.y, alert.adj_node[n].temp, mac_arr[rank]);
    }
    if (found == 1)
    {
        fprintf(bs_log, "\nInfrared Satellite Reporting Time: %d-%02d-%02d %02d:%02d:%02d\n", log_time.tm_year + 1900, log_time.tm_mon + 1, log_time.tm_mday, log_time.tm_hour, log_time.tm_min, log_time.tm_sec);
        fprintf(bs_log, "Infrared Satellite Reporting (Celsius): %d\n", satellite_node.temp);
        fprintf(bs_log, "Infrared Satellite Reporting Coord: (%d, %d)\n", satellite_node.coord.x, satellite_node.coord.y);
    }

    fprintf(bs_log, "Communication Time (seconds): %lf\n", t_elapsed);
    fprintf(bs_log, "Total Messages send between reporting node and base station: %d\n", mp_count);
    fprintf(bs_log, "Number of adjacent matches to reporting node: %d\n", overheat_node_count);
    fprintf(bs_log, "------------------------------------------------------\n");
    fclose(bs_log);
}

void log_summary(int *mp_total_counter, int *mp_true_counter, int *mp_sat_false_counter, int *mp_false_counter, double avg_time, int n)
{
    FILE *bs_log;
    bs_log = fopen("base_station_log.txt", "a");

    int total_alert = getSum(mp_total_counter, n);
    int true_alert = getSum(mp_true_counter, n);
    int sat_false_alert = getSum(mp_sat_false_counter, n);
    int false_alert = getSum(mp_false_counter, n);

    fprintf(bs_log, "------------------------------------------------------\n");
    fprintf(bs_log, "SUMMARY\n");
    fprintf(bs_log, "Total number of alerts: %d\n", total_alert);
    fprintf(bs_log, "Number of true alerts: %d\n", true_alert);
    fprintf(bs_log, "Number of false alerts: %d\n", sat_false_alert);
    fprintf(bs_log, "Number of false (no infrared satellite reading) alerts: %d\n", false_alert);
    fprintf(bs_log, "Average communication time (s): %f\n", avg_time);
    fprintf(bs_log, "------------------------------------------------------\n");
    fclose(bs_log);
}

int getSum(int *arr, int n)
{
    int sum = 0;
    for (int i = 0; i < n; i++)
    {
        sum += arr[i];
    }
    return sum;
}

int getMac(unsigned char *mac)
{
    int fd;
    struct ifreq ifr;
    char *iface = "enp0s3";
    unsigned char *mac_arr = NULL;

    memset(&ifr, 0, sizeof(ifr));

    fd = socket(AF_INET, SOCK_DGRAM, 0);

    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    if (0 == ioctl(fd, SIOCGIFHWADDR, &ifr))
    {
        mac_arr = (unsigned char *)ifr.ifr_hwaddr.sa_data;
        sprintf(mac, "%.2X:%.2X:%.2X:%.2X:%.2X:%.2X\n", mac_arr[0], mac_arr[1], mac_arr[2], mac_arr[3], mac_arr[4], mac_arr[5]);
    }

    close(fd);

    return 0;
}