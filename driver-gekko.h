#include "math.h"
#include "miner.h"
#include "usbutils.h"

#define TX_TASK_SIZE 64          // BM1384 Work Task Request Size
#define RX_RESP_SIZE  5          // BM1384 Response Size

#define RAMP_CT 55               // Step counts to take when ramping
#define RAMP_MS 33               // MS between ramp, total ramp time = RAMP_CT x RAMP_MS / 1000

#define MAX_JOBS 0x1F            // Max BM1384 Job Id
#define MAX_IDLE 60              // No nonces seconds before we consider device non functional.

#define BASE_FREQ 50             // Reference point frequency

struct COMPAC_INFO {

	enum sub_ident ident;        // Miner identity
	struct thr_info *thr;        // Running Thread
	struct thr_info rthr;        // Listening Thread

	pthread_mutex_t lock;        // Mutex

	float frequency;             // Chip Frequency
	float frequency_requested;   // Requested Frequency
	float frequency_start;       // Starting Frequency
	
	int start_freq;
	int step_delay;
	int step_freq;

	uint32_t scanhash_ms;        // Avg time(ms) inside scanhash loop
	uint32_t task_ms;            // Avg time(ms) between task sent to device
	uint32_t fullscan_ms;        // Estimated time(ms) for full nonce range
	uint64_t hashrate;           // Estimated hashrate = 55M x Chips x Frequency

	uint64_t ramp_hcn;           // HCN high watermark at ramping
	uint32_t prev_nonce;         // Last nonce found

	int failing;                 // Flag failing sticks
	int write_err;               // Consecutive usb write errors
	bool active;                 // Done ramping, send live work and get nonces

	int accepted;                // Nonces accepted
	int nonces;                  // Nonces found
	int dups;                    // Duplicates found
	int nonceless;               // Tasks sent.  Resets when nonce is found.
	uint64_t hashes;             // Hashes completed
	int interface;               // USB interface

	uint32_t ticket_mask;        // Used to reduce flashes per second
	uint32_t difficulty;         // For computing hashrate
	uint32_t chips;              // Stores number of chips found
	uint32_t ramping;            // Ramping incrementer
	uint32_t update_work;        // Notification of work update
	uint32_t job_id;             // JobId incrementer

	struct timeval start_time;              // Device startup time
	struct timeval last_scanhash;           // Last time inside scanhash loop
	struct timeval last_task;               // Last time work was sent
	struct timeval last_nonce;              // Last time nonce was found
	struct timeval last_hwerror;            // Last time hw error was detected
	struct timeval last_freq_set;           // Last change of frequency
	struct timeval last_chain_inactive;     // Last sent chain inactive

	unsigned char work_tx[TX_TASK_SIZE];    // Task transmit buffer
	unsigned char work_rx[RX_RESP_SIZE];    // Receive buffer

	struct work *work[MAX_JOBS];            // Work ring buffer

};

void stuff_int32(unsigned char *dst, uint32_t x);
void stuff_reverse(unsigned char *dst, unsigned char *src, uint32_t len);
uint64_t bound(uint64_t value, uint64_t lower_bound, uint64_t upper_bound);