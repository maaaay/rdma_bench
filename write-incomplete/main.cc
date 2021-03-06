#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <thread>
#include <vector>
#include "libhrd_cpp/hrd.h"

/// The value of counter is x iff the client has completed writing x to the
/// server's memory. This is shared between the client and server.
std::atomic<size_t> shared_counter;

/// The client and server ports must be on different NICs for the
/// proof-of-concept to work.
static size_t kClientRDMAPort = 0;
static size_t kServerRDMAPort = 1;

void run_server() {
  // Establish a QP with the client
  struct hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = false;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = sizeof(size_t);
  conn_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(0 /* id */, kServerRDMAPort, kHrdInvalidNUMANode,
                               &conn_config, nullptr /* datagram config */);
  memset(const_cast<uint8_t*>(cb->conn_buf), 0, sizeof(size_t));

  hrd_publish_conn_qp(cb, 0, "server");
  printf("main: Server published. Waiting for client.\n");

  hrd_qp_attr_t* clt_qp = nullptr;
  while (clt_qp == nullptr) {
    clt_qp = hrd_get_published_qp("client");
    if (clt_qp == nullptr) usleep(200000);
  }

  printf("main: Server found client! Connecting..\n");
  hrd_connect_qp(cb, 0, clt_qp);
  hrd_publish_ready("server");
  printf("main: Server ready\n");

  // Start real work
  auto* ptr = reinterpret_cast<volatile size_t*>(cb->conn_buf);
  while (true) {
    size_t minimum_allowed = shared_counter;

    asm volatile("" ::: "memory");        // Compiler barrier
    asm volatile("mfence" ::: "memory");  // Hardware barrier

    // Check if the client's write is actually visible
    size_t actual = ptr[0];
    if (actual < minimum_allowed) {
      printf("violation: actual = %zu, minimum_allowed = %zu\n", actual,
             minimum_allowed);
    }
  }
}

void run_client() {
  // Establish a QP with the server
  hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = false;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = sizeof(size_t);
  conn_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(1 /* id */, kClientRDMAPort, kHrdInvalidNUMANode,
                               &conn_config, nullptr /* datagram config */);

  hrd_publish_conn_qp(cb, 0, "client");
  printf("main: Client published. Waiting for server.\n");

  hrd_qp_attr_t* srv_qp = nullptr;
  while (srv_qp == nullptr) {
    srv_qp = hrd_get_published_qp("server");
    if (srv_qp == nullptr) usleep(2000);
  }

  printf("main: Client found server. Connecting..\n");
  hrd_connect_qp(cb, 0, srv_qp);
  printf("main: Client connected!\n");

  hrd_wait_till_ready("server");

  // Start real work
  struct ibv_send_wr wr, *bad_send_wr;
  struct ibv_sge sge;
  struct ibv_wc wc;

  auto* ptr = reinterpret_cast<volatile size_t*>(&cb->conn_buf[0]);
  while (true) {
    ptr[0] = shared_counter + 1;

    sge.addr = reinterpret_cast<uint64_t>(cb->conn_buf);
    sge.length = sizeof(size_t);
    sge.lkey = cb->conn_buf_mr->lkey;

    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.num_sge = 1;
    wr.next = nullptr;
    wr.sg_list = &sge;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = srv_qp->buf_addr;
    wr.wr.rdma.rkey = srv_qp->rkey;

    int ret = ibv_post_send(cb->conn_qp[0], &wr, &bad_send_wr);
    rt_assert(ret == 0);
    hrd_poll_cq(cb->conn_cq[0], 1, &wc);  // Block till the RDMA write completes

    asm volatile("" ::: "memory");        // Compiler barrier
    asm volatile("mfence" ::: "memory");  // Hardware barrier

    shared_counter++;  // The RDMA write is complete, so server must see update
  }
}

int main() {
  shared_counter = 0;

  auto thread_server = std::thread(run_server);
  auto thread_client = std::thread(run_client);

  thread_server.join();
  thread_client.join();
  return 0;
}
