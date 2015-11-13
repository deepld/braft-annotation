/*
 * =====================================================================================
 *
 *       Filename:  node.cpp
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  2015/10/08 17:00:15
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  WangYao (fisherman), wangyao02@baidu.com
 *        Company:  Baidu, Inc
 *
 * =====================================================================================
 */

#include "raft/util.h"
#include "raft/raft.h"
#include "raft/node.h"
#include "raft/log.h"
#include "raft/stable.h"
#include "raft/snapshot.h"
#include "raft/file_service.h"
#include <bthread_unstable.h>

#include "baidu/rpc/errno.pb.h"
#include "baidu/rpc/controller.h"
#include "baidu/rpc/channel.h"

namespace raft {

NodeImpl::NodeImpl(const GroupId& group_id, const ReplicaId& replica_id)
    : _group_id(group_id),
    _state(SHUTDOWN), _current_term(0),
    _last_snapshot_term(0), _last_snapshot_index(0),
    _last_leader_timestamp(base::monotonic_time_ms()),
    _snapshot_saving(false), _loading_snapshot_meta(NULL),
    _log_storage(NULL), _stable_storage(NULL), _snapshot_storage(NULL),
    _config_manager(NULL), _log_manager(NULL),
    _fsm_caller(NULL), _commit_manager(NULL) {

        _server_id = PeerId(NodeManager::GetInstance()->address(), replica_id);
        AddRef();
        bthread_mutex_init(&_mutex, NULL);
}

NodeImpl::~NodeImpl() {
    bthread_mutex_destroy(&_mutex);

    if (_config_manager) {
        _config_manager->Release();
        _config_manager = NULL;
    }
    if (_log_manager) {
        delete _log_manager;
        _log_manager = NULL;
    }
    if (_fsm_caller) {
        delete _fsm_caller;
        _fsm_caller = NULL;
    }
    if (_commit_manager) {
        delete _commit_manager;
        _commit_manager = NULL;
    }

    if (_log_storage) {
        delete _log_storage;
        _log_storage = NULL;
    }
    if (_stable_storage) {
        delete _stable_storage;
        _stable_storage = NULL;
    }
    if (_snapshot_storage) {
        delete _snapshot_storage;
        _snapshot_storage = NULL;
    }
}

void NodeImpl::on_snapshot_load_done() {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    CHECK(_loading_snapshot_meta);

    _last_snapshot_index = _loading_snapshot_meta->last_included_index;
    _last_snapshot_term = _loading_snapshot_meta->last_included_term;
    // Check discard entire log
    // 1. Discard log if it is shorter than the snapshot.
    // 2. Discard log if its lastSnapshotIndex entry disagrees with the
    //    lastSnapshotTerm.
    if (_log_manager->last_log_index() < _last_snapshot_index ||
        (_log_manager->first_log_index() <= _last_snapshot_index &&
         _log_manager->get_term(_last_snapshot_index) != _last_snapshot_term)) {
        if (_log_manager->first_log_index() <= _log_manager->last_log_index()) {
            LOG(WARNING) << "node " << _group_id << ":" << _server_id
                << " discard the entire log, it is consistent with installed snapshot";
        }
        // discard entire log
        _log_manager->truncate_prefix(_last_snapshot_index + 1);
        _log_manager->truncate_suffix(_last_snapshot_index);
    }

    // discard unneed entries before _last_snapshot_index
    if (_log_manager->first_log_index() <= _last_snapshot_index) {
        _log_manager->truncate_prefix(_last_snapshot_index + 1);
    }

    // update configuration
    _config_manager->set_snapshot(_loading_snapshot_meta->last_included_index,
                                  _loading_snapshot_meta->last_configuration);
    _log_manager->check_and_set_configuration(_conf);

    // reset commit manager
    _commit_manager->reset_pending_index(_loading_snapshot_meta->last_included_index + 1);

    delete _loading_snapshot_meta;
    _loading_snapshot_meta = NULL;
}

int NodeImpl::on_snapshot_save_done(const int64_t last_included_index, SnapshotWriter* writer) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);
    int ret = 0;

    do {
        // InstallSnapshot can break SaveSnapshot, check InstallSnapshot when SaveSnapshot
        // because upstream Snapshot mybe newer than local Snapshot.
        if (last_included_index <= _last_snapshot_index) {
            ret = ESTALE;
            LOG(WARNING) << "node " << _group_id << ":" << _server_id
                << " discard saved snapshot, because has a newer snapshot."
                << " last_included_index " << last_included_index
                << " last_snapshot_index " << _last_snapshot_index;
            writer->set_error(ESTALE, "snapshot is staled, maybe InstallSnapshot when snapshot");
            break;
        }

        CHECK(last_included_index >= _log_manager->first_log_index());
        CHECK(last_included_index <= _log_manager->last_log_index());

        _last_snapshot_index = last_included_index;
        _last_snapshot_term = _log_manager->get_term(last_included_index);

        // set snapshot to configration
        ConfigurationPair pair = _config_manager->get_configuration(last_included_index);
        if (pair.first != 0) {
            _config_manager->set_snapshot(pair.first, pair.second);
        }

        // discard unneed entries before _last_snapshot_index
        // OPTIMIZE: should be defer discard entries when some followers are catching up.
        if (_log_manager->first_log_index() <= _last_snapshot_index) {
            _log_manager->truncate_prefix(_last_snapshot_index + 1);
        }

        //TODO: discard unneed snapshot

        // update configuration
        _log_manager->check_and_set_configuration(_conf);

        ret = writer->save_meta();
    } while (0);

    _snapshot_saving = false;
    return ret;
}

int NodeImpl::init_snapshot_storage() {
    int ret = 0;
    SnapshotReader* snapshot_reader = NULL;

    do {
        if (_options.snapshot_uri.empty()) {
            break;
        }

        Storage* storage = find_storage(_options.snapshot_uri);
        if (storage) {
            _snapshot_storage = storage->create_snapshot_storage(
                    _options.snapshot_uri);
        } else {
            LOG(WARNING) << "node " << _group_id << ":" << _server_id
                << " find snapshot storage failed, uri " << _options.snapshot_uri;
            ret = ENOENT;
            break;
        }

        ret = _snapshot_storage->init();
        if (ret != 0) {
            LOG(WARNING) << "node " << _group_id << ":" << _server_id
                << " init snapshot storage failed, uri " << _options.snapshot_uri;
            ret = EINVAL;
            break;
        }

        // read snapshot
        snapshot_reader = _snapshot_storage->open();
        if (snapshot_reader) {
            // fsm load snapshot in current bthread
            ret = _options.fsm->on_snapshot_load(snapshot_reader);
            if (ret != 0) {
                LOG(WARNING) << "node " << _group_id << ":" << _server_id
                    << " fsm load snapshot failed, uri " << _options.snapshot_uri;
                break;
            }

            // load meta
            _loading_snapshot_meta = new SnapshotMeta();
            ret = snapshot_reader->load_meta(_loading_snapshot_meta);
            if (ret == 0) {
                on_snapshot_load_done();
            } else {
                LOG(WARNING) << "node " << _group_id << ":" << _server_id
                    << " load snapshot meta failed, uri " << _options.snapshot_uri;
                delete _loading_snapshot_meta;
                _loading_snapshot_meta = NULL;
                break;
            }
        } else {
            LOG(INFO) << "node " << _group_id << ":" << _server_id
                << " snapshot storage empty, uri " << _options.snapshot_uri;
        }
    } while (0);

    if (_snapshot_storage && snapshot_reader) {
        _snapshot_storage->close(snapshot_reader);
    }
    return ret;
}

int NodeImpl::init_log_storage() {
    int ret = 0;

    do {
        Storage* storage = find_storage(_options.log_uri);
        if (storage) {
            _log_storage = storage->create_log_storage(_options.log_uri);
        } else {
            ret = ENOENT;
            break;
        }

        _log_manager = new LogManager();
        LogManagerOptions log_manager_options;
        log_manager_options.log_storage = _log_storage;
        log_manager_options.configuration_manager = _config_manager;
        ret = _log_manager->init(log_manager_options);
        if (ret != 0) {
            break;
        }
    } while (0);

    return ret;
}

int NodeImpl::init_stable_storage() {
    int ret = 0;

    do {
        Storage* storage = find_storage(_options.stable_uri);
        if (storage) {
            _stable_storage = storage->create_stable_storage(_options.stable_uri);
        } else {
            LOG(WARNING) << "node " << _group_id << ":" << _server_id
                << " find stable storage failed, uri " << _options.stable_uri;
            ret = ENOENT;
            break;
        }

        ret = _stable_storage->init();
        if (ret != 0) {
            LOG(WARNING) << "node " << _group_id << ":" << _server_id
                << " int stable storage failed, uri " << _options.stable_uri
                << " ret " << ret;
            break;
        }

        _current_term = _stable_storage->get_term();
        ret = _stable_storage->get_votedfor(&_voted_id);
        if (ret != 0) {
            LOG(WARNING) << "node " << _group_id << ":" << _server_id
                << " stable storage get_votedfor failed, uri " << _options.stable_uri
                << " ret " << ret;
            break;
        }
    } while (0);

    return ret;
}

static void on_snapshot_timer(void* arg) {
    NodeImpl* node = static_cast<NodeImpl*>(arg);

    node->handle_snapshot_timeout();
    node->Release();
}

void NodeImpl::handle_snapshot_timeout() {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    // check state
    if (_state == SHUTDOWN) {
        return;
    }

    snapshot(NULL);

    AddRef();
    bthread_timer_add(&_snapshot_timer, base::milliseconds_from_now(_options.snapshot_interval),
                      on_snapshot_timer, this);
    RAFT_VLOG << "node " << _group_id << ":" << _server_id
        << " term " << _current_term << " restart snapshot_timer";
}

int NodeImpl::init(const NodeOptions& options) {
    if (NodeManager::GetInstance()->address() == base::EndPoint(base::IP_ANY, 0)) {
        LOG(ERROR) << "Raft Server not inited.";
        return EINVAL;
    }

    int ret = 0;

    _options = options;

    _config_manager = new ConfigurationManager();

    do {
        // log storage and log manager init
        ret = init_log_storage();
        if (0 != ret) {
            break;
        }

        // stable init
        ret = init_stable_storage();
        if (0 != ret) {
            break;
        }

        // snapshot storage init and load
        // NOTE: snapshot maybe discard entries when snapshot saved but not discard entries.
        //      init log storage before snapshot storage, snapshot storage will update configration
        ret = init_snapshot_storage();
        if (0 != ret) {
            break;
        }

        // if have log using conf in log, else using conf in options
        if (_log_manager->last_log_index() > 0) {
            _log_manager->check_and_set_configuration(_conf);
        } else {
            _conf.second = _options.conf;
        }

        // fsm caller init, node AddRef in init
        _fsm_caller = new FSMCaller();
        FSMCallerOptions fsm_caller_options;
        fsm_caller_options.last_applied_index = _last_snapshot_index;
        fsm_caller_options.node = this;
        fsm_caller_options.log_manager = _log_manager;
        fsm_caller_options.fsm = _options.fsm;
        ret = _fsm_caller->init(fsm_caller_options);
        if (ret != 0) {
            break;
        }

        //TODO: call fsm on_apply (_last_snapshot_index + 1, last_committed_index]
        int64_t last_committed_index = _last_snapshot_index;

        // commitment manager init
        _commit_manager = new CommitmentManager();
        CommitmentManagerOptions commit_manager_options;
        commit_manager_options.max_pending_size = 1000; //TODO0
        commit_manager_options.waiter = _fsm_caller;
        commit_manager_options.last_committed_index = last_committed_index;
        ret = _commit_manager->init(commit_manager_options);
        if (ret != 0) {
            // fsm caller init AddRef, here Release
            Release();
            break;
        }

        // add node to NodeManager
        if (!NodeManager::GetInstance()->add(this)) {
            LOG(WARNING) << "NodeManager add " << _group_id << ":" << _server_id << "failed, exist";
            ret = EEXIST;
            break;
        }

        // set state to follower
        _state = FOLLOWER;
        LOG(INFO) << "node " << _group_id << ":" << _server_id << " init,"
            << " term: " << _current_term
            << " last_log_index: " << _log_manager->last_log_index()
            << " conf: " << _conf.second;
        if (!_conf.second.empty()) {
            step_down(_current_term);
        }

        // start snapshot timer
        if (_snapshot_storage && _options.snapshot_interval > 0) {
            AddRef();
            bthread_timer_add(&_snapshot_timer,
                              base::milliseconds_from_now(_options.snapshot_interval),
                              on_snapshot_timer, this);
            RAFT_VLOG << "node " << _group_id << ":" << _server_id
                << " term " << _current_term << " start snapshot_timer";
        }
    } while (0);

    return ret;
}

void NodeImpl::apply(const base::IOBuf& data, Closure* done) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    if (_state == SHUTDOWN) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " not inited";
        _fsm_caller->on_cleared(0, done, EINVAL);
        return;
    }
    if (_state != LEADER) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " can't apply not in LEADER";
        _fsm_caller->on_cleared(0, done, EPERM);
        return;
    }

    LogEntry* entry = new LogEntry;
    entry->term = _current_term;
    entry->type = ENTRY_TYPE_DATA;
    entry->data.append(data);
    append(entry, done);
}

void NodeImpl::on_configuration_change_done(const EntryType type,
                                            const std::vector<PeerId>& new_peers) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    if (type == ENTRY_TYPE_ADD_PEER) {
        LOG(INFO) << "node " << _group_id << ":" << _server_id << " add_peer to "
            << _conf.second << " success.";
    } else if (type == ENTRY_TYPE_REMOVE_PEER) {
        LOG(INFO) << "node " << _group_id << ":" << _server_id << " remove_peer to "
            << _conf.second << " success.";

        ConfigurationCtx old_conf_ctx = _conf_ctx;
        // remove_peer will stop peer replicator or shutdown
        if (!_conf.second.contain(_server_id)) {
            //TODO: shutdown?
            _conf.second.reset();
            step_down(_current_term);
        } else {
            Configuration old_conf(_conf_ctx.peers);
            for (size_t i = 0; i < new_peers.size(); i++) {
                old_conf.remove_peer(new_peers[i]);
            }
            std::vector<PeerId> removed_peers;
            old_conf.peer_vector(&removed_peers);
            for (size_t i = 0; i < removed_peers.size(); i++) {
                _replicator_group.stop_replicator(removed_peers[i]);
            }
        }
    }
    _conf_ctx.reset();
}

static void on_peer_caught_up(void* arg, const PeerId& pid, const int error_code, Closure* done) {
    NodeImpl* node = static_cast<NodeImpl*>(arg);
    node->on_caughtup(pid, error_code, done);
    node->Release();
}

void NodeImpl::on_caughtup(const PeerId& peer, int error_code, Closure* done) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    if (error_code == 0) {
        LOG(INFO) << "node " << _group_id << ":" << _server_id << " add_peer " << peer
            << " to " << _conf.second << ", caughtup "
            << "success, then append add_peer entry.";
        // add peer to _conf after new peer catchup
        Configuration new_conf(_conf.second);
        new_conf.add_peer(peer);

        LogEntry* entry = new LogEntry();
        entry->term = _current_term;
        entry->type = ENTRY_TYPE_ADD_PEER;
        entry->peers = new std::vector<PeerId>;
        new_conf.peer_vector(entry->peers);
        append(entry, done);
        return;
    }

    if (error_code == ETIMEDOUT &&
        (base::monotonic_time_ms() -  _replicator_group.last_response_timestamp(peer)) <=
        _options.election_timeout) {

        LOG(INFO) << "node " << _group_id << ":" << _server_id << " catching up " << peer;

        AddRef();
        OnCaughtUp caught_up;
        timespec due_time = base::microseconds_from_now(_options.election_timeout);
        caught_up.on_caught_up = on_peer_caught_up;
        caught_up.done = done;
        caught_up.arg = this;
        caught_up.min_margin = 1000; //TODO0

        if (0 == _replicator_group.wait_caughtup(peer, caught_up, &due_time)) {
            return;
        } else {
            LOG(ERROR) << "wait_caughtup failed, peer " << peer;
            Release();
        }
    }

    LOG(INFO) << "node " << _group_id << ":" << _server_id << " add_peer " << peer
        << " to " << _conf.second << ", caughtup "
        << "failed: " << error_code;

    // call user function, on_caught_up run in bthread created in replicator
    Configuration old_conf(_conf_ctx.peers);
    done->set_error(error_code, "caughtup failed");
    done->Run();
    _conf_ctx.reset();

    // stop_replicator after call user function, make bthread_id in replicator available
    _replicator_group.stop_replicator(peer);
}

static void on_stepdown_timer(void* arg) {
    NodeImpl* node = static_cast<NodeImpl*>(arg);

    node->handle_stepdown_timeout();
    node->Release();
}

void NodeImpl::handle_stepdown_timeout() {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    // check state
    if (_state != LEADER) {
        return;
    }

    std::vector<PeerId> peers;
    _conf.second.peer_vector(&peers);
    int64_t now_timestamp = base::monotonic_time_ms();
    size_t dead_count = 0;
    for (size_t i = 0; i < peers.size(); i++) {
        if (peers[i] == _server_id) {
            continue;
        }

        if (now_timestamp - _replicator_group.last_response_timestamp(peers[i]) >
            _options.election_timeout) {
            dead_count++;
        }
    }
    if (dead_count < (peers.size() / 2 + 1)) {
        AddRef();
        int64_t stepdown_timeout = _options.election_timeout;
        bthread_timer_add(&_stepdown_timer, base::milliseconds_from_now(stepdown_timeout),
                          on_stepdown_timer, this);
        RAFT_VLOG << "node " << _group_id << ":" << _server_id
            << " term " << _current_term << " restart stepdown_timer";
    } else {
        LOG(INFO) << "node " << _group_id << ":" << _server_id
            << " term " << _current_term << " stepdown when quorum node dead";
        step_down(_current_term);
    }
}

void NodeImpl::add_peer(const std::vector<PeerId>& old_peers, const PeerId& peer,
                       Closure* done) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    // check state
    if (_state != LEADER) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " can't apply not in LEADER";
        _fsm_caller->on_cleared(0, done, EPERM);
        return;
    }
    // check concurrent conf change
    if (!_conf_ctx.empty()) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " remove_peer need wait "
            "current conf change";
        _fsm_caller->on_cleared(0, done, EINVAL);
        return;
    }
    // check equal
    if (!_conf.second.equal(old_peers)) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " add_peer dismatch old_peers";
        _fsm_caller->on_cleared(0, done, EINVAL);
        return;
    }
    // check contain
    if (_conf.second.contain(peer)) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " add_peer old_peers "
            "contains new_peer";
        _fsm_caller->on_cleared(0, done, EINVAL);
        return;
    }

    LOG(INFO) << "node " << _group_id << ":" << _server_id << " add_peer " << peer
        << " to " << _conf.second << ", begin caughtup.";

    if (0 != _replicator_group.add_replicator(peer)) {
        LOG(ERROR) << "start replicator failed, peer " << peer;
        _fsm_caller->on_cleared(0, done, EINVAL);
        return;
    }

    // catch up new peer
    AddRef();
    OnCaughtUp caught_up;
    timespec due_time = base::microseconds_from_now(_options.election_timeout);
    caught_up.on_caught_up = on_peer_caught_up;
    caught_up.done = done;
    caught_up.arg = this;
    caught_up.min_margin = 1000; //TODO0

    //TODO: check return code
    if (0 != _replicator_group.wait_caughtup(peer, caught_up, &due_time)) {
        LOG(ERROR) << "wait_caughtup failed, peer " << peer;
        Release();
        _fsm_caller->on_cleared(0, done, EINVAL);
        return;
    }
}

void NodeImpl::remove_peer(const std::vector<PeerId>& old_peers, const PeerId& peer,
                          Closure* done) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    // check state
    if (_state != LEADER) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " can't apply not in LEADER";
        _fsm_caller->on_cleared(0, done, EPERM);
        return;
    }
    // check concurrent conf change
    if (!_conf_ctx.empty()) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " remove_peer need wait "
            "current conf change";
        _fsm_caller->on_cleared(0, done, EAGAIN);
        return;
    }
    // check equal
    if (!_conf.second.equal(old_peers)) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id
            << " remove_peer dismatch old_peers";
        _fsm_caller->on_cleared(0, done, EINVAL);
        return;
    }
    // check contain
    if (!_conf.second.contain(peer)) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " remove_peer old_peers not "
            "contains new_peer";
        _fsm_caller->on_cleared(0, done, EINVAL);
        return;
    }

    LOG(INFO) << "node " << _group_id << ":" << _server_id << " remove_peer " << peer
        << " from " << _conf.second;

    Configuration new_conf(_conf.second);
    new_conf.remove_peer(peer);

    // remove peer from _conf when REMOVE_PEER committed, shutdown when remove leader self
    LogEntry* entry = new LogEntry();
    entry->term = _current_term;
    entry->type = ENTRY_TYPE_REMOVE_PEER;
    entry->peers = new std::vector<PeerId>;
    new_conf.peer_vector(entry->peers);
    append(entry, done);
}

int NodeImpl::set_peer(const std::vector<PeerId>& old_peers, const std::vector<PeerId>& new_peers) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    // check state
    if (_state == SHUTDOWN) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " not inited";
        return EINVAL;
    }
    // check bootstrap
    if (_conf.second.empty() && old_peers.size() == 0) {
        Configuration new_conf(new_peers);
        LOG(INFO) << "node " << _group_id << ":" << _server_id << " set_peer boot from "
            << new_conf;
        _conf.second = new_conf;
        step_down(1);
        return 0;
    }
    // check concurrent conf change
    if (_state == LEADER && !_conf_ctx.empty()) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " remove_peer need wait "
            "current conf change";
        return EINVAL;
    }
    // check equal
    if (!_conf.second.equal(std::vector<PeerId>(old_peers))) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " set_peer dismatch old_peers";
        return EINVAL;
    }
    // check quorum
    if (new_peers.size() >= (old_peers.size() / 2 + 1)) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " set_peer new_peers greater "
            "than old_peers'quorum";
        return EINVAL;
    }
    // check contain
    if (!_conf.second.contain(new_peers)) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " set_peer old_peers not "
            "contains all new_peers";
        return EINVAL;
    }

    Configuration new_conf(new_peers);
    LOG(INFO) << "node " << _group_id << ":" << _server_id << " set_peer from " << _conf.second
        << " to " << new_conf;
    // step down and change conf
    step_down(_current_term + 1);
    _conf.second = new_conf;
    return 0;
}

SaveSnapshotDone::SaveSnapshotDone(NodeImpl* node, SnapshotStorage* snapshot_storage, Closure* done)
    : _node(node), _snapshot_storage(snapshot_storage), _writer(NULL),
    _done(done) {
    _node->AddRef();
}

SaveSnapshotDone::~SaveSnapshotDone() {
    _node->Release();
}

SnapshotWriter* SaveSnapshotDone::start(const SnapshotMeta& meta) {
    _meta = meta;
    _writer = _snapshot_storage->create(meta);
    return _writer;
}

void SaveSnapshotDone::Run() {
    if (_err_code == 0) {
        int ret = _node->on_snapshot_save_done(_meta.last_included_index, _writer);
        if (ret != 0) {
            set_error(ret, "node call on_snapshot_save_done failed");
        }
    }

    // close writer
    if (_writer) {
        _snapshot_storage->close(_writer);
    }

    //user done, need set error
    if (_err_code != 0) {
        _done->set_error(_err_code, _err_text.c_str());
    }
    _done->Run();
    delete this;
}

void NodeImpl::snapshot(Closure* done) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    // check state
    if (_state == SHUTDOWN) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id << " not inited";
        _fsm_caller->on_cleared(0, done, EINVAL);
        return;
    }

    // check support snapshot?
    if (NULL == _snapshot_storage) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id
            << " unsupport snapshot, maybe snapshot_uri not set";
        _fsm_caller->on_cleared(0, done, EINVAL);
        return;
    }

    // check snapshot install/load
    if (_loading_snapshot_meta) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id
            << " doing snapshot load/install";
        _fsm_caller->on_cleared(0, done, EAGAIN);
        return;
    }

    // check snapshot saving?
    if (_snapshot_saving) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id
            << " doing snapshot save";
        _fsm_caller->on_cleared(0, done, EAGAIN);
        return;
    }

    _snapshot_saving = true;
    //TODO:
    SaveSnapshotDone* snapshot_save_done = new SaveSnapshotDone(this, _snapshot_storage, done);
    _fsm_caller->on_snapshot_save(snapshot_save_done);
}

void NodeImpl::shutdown(Closure* done) {
    // remove node from NodeManager, rpc will not touch Node
    NodeManager::GetInstance()->remove(this);

    std::lock_guard<bthread_mutex_t> guard(_mutex);

    LOG(INFO) << "node " << _group_id << ":" << _server_id << " shutdown,"
        " current_term " << _current_term << " state " << State2Str(_state);

    // leader stop disk thread and replicator, stop stepdown timer, change state to FOLLOWER
    // candidate stop vote timer, change state to FOLLOWER
    if (_state != FOLLOWER) {
        step_down(_current_term);
    }

    // follower stop election timer
    RAFT_VLOG << "node " << _group_id << ":" << _server_id
        << " term " << _current_term << " stop election_timer";
    int ret = bthread_timer_del(_election_timer);
    if (ret == 0) {
        Release();
    }

    // all stop snapshot timer
    RAFT_VLOG << "node " << _group_id << ":" << _server_id
        << " term " << _current_term << " stop snapshot_timer";
    ret = bthread_timer_del(_snapshot_timer);
    if (ret == 0) {
        Release();
    }

    // change state to shutdown
    _state = SHUTDOWN;

    // stop replicator and fsm_caller wait
    _log_manager->shutdown();

    // step_down will call _commitment_manager->clear_pending_applications(),
    // this can avoid send LogEntry with closure to fsm_caller.
    // fsm_caller shutdown will not leak user's closure.
    _fsm_caller->shutdown(done);
}

static void on_election_timer(void* arg) {
    NodeImpl* node = static_cast<NodeImpl*>(arg);

    node->handle_election_timeout();
    node->Release();
}

void NodeImpl::handle_election_timeout() {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    // check state
    if (_state != FOLLOWER) {
        return;
    }
    // check timestamp
    if (base::monotonic_time_ms() - _last_leader_timestamp < _options.election_timeout) {
        AddRef();
        int64_t election_timeout = random_timeout(_options.election_timeout);
        bthread_timer_add(&_election_timer, base::milliseconds_from_now(election_timeout),
                          on_election_timer, this);
        RAFT_VLOG << "node " << _group_id << ":" << _server_id
            << " term " << _current_term << " restart elect_timer";
        return;
    }

    // first vote
    RAFT_VLOG << "node " << _group_id << ":" << _server_id
        << " term " << _current_term << " start elect";
    elect_self();
}

static void on_vote_timer(void* arg) {
    NodeImpl* node = static_cast<NodeImpl*>(arg);

    node->handle_vote_timeout();
    node->Release();
}

void NodeImpl::handle_vote_timeout() {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    // check state
    if (_state == CANDIDATE) {
        // retry vote
        RAFT_VLOG << "node " << _group_id << ":" << _server_id
            << " term " << _current_term << " retry elect";
        elect_self();
    }
}

void NodeImpl::handle_request_vote_response(const PeerId& peer_id, const int64_t term,
                                            const RequestVoteResponse& response) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    // check state
    if (_state != CANDIDATE) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id
            << " received invalid RequestVoteResponse from " << peer_id
            << " state not in CANDIDATE";
        return;
    }
    // check stale response
    if (term != _current_term) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id
            << " received stale RequestVoteResponse from " << peer_id
            << " term " << term << " current_term " << _current_term;
        return;
    }
    // check response term
    if (response.term() > _current_term) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id
            << " received invalid RequestVoteResponse from " << peer_id
            << " term " << response.term() << " expect " << _current_term;
        step_down(response.term());
        return;
    }

    LOG(INFO) << "node " << _group_id << ":" << _server_id
        << " received RequestVoteResponse from " << peer_id
        << " term " << response.term() << " granted " << response.granted();
    // check granted quorum?
    if (response.granted()) {
        _vote_ctx.grant(peer_id);
        if (_vote_ctx.quorum()) {
            become_leader();
        }
    }
}

struct OnRequestVoteRPCDone : public google::protobuf::Closure {
    OnRequestVoteRPCDone(const PeerId& peer_id_, const int64_t term_, NodeImpl* node_)
        : peer(peer_id_), term(term_), node(node_) {
            node->AddRef();
    }
    virtual ~OnRequestVoteRPCDone() {
        node->Release();
    }

    void Run() {
        do {
            if (cntl.ErrorCode() != 0) {
                LOG(WARNING) << "node " << node->node_id()
                    << " RequestVote to " << peer << " error: " << cntl.ErrorText();
                break;
            }
            node->handle_request_vote_response(peer, term, response);
        } while (0);
        delete this;
    }

    PeerId peer;
    int64_t term;
    RequestVoteResponse response;
    baidu::rpc::Controller cntl;
    NodeImpl* node;
};

// in lock
int64_t NodeImpl::last_log_term() {
    int64_t term = 0;
    int64_t last_log_index = _log_manager->last_log_index();
    if (last_log_index >= _log_manager->first_log_index()) {
        term = _log_manager->get_term(last_log_index);
    } else {
        term = _last_snapshot_term;
    }
    return term;
}

// in lock
void NodeImpl::elect_self() {
    LOG(INFO) << "node " << _group_id << ":" << _server_id
        << " term " << _current_term
        << " start vote and grant vote self";
    // cancel follower election timer
    if (_state == FOLLOWER) {
        RAFT_VLOG << "node " << _group_id << ":" << _server_id
            << " term " << _current_term << " stop elect_timer";
        int ret = bthread_timer_del(_election_timer);
        if (ret == 0) {
            Release();
        } else {
            CHECK(ret == 1);
        }
    }
    _state = CANDIDATE;
    _current_term++;
    _voted_id = _server_id;
    _vote_ctx.reset();

    AddRef();
    int64_t vote_timeout = random_timeout(std::max(_options.election_timeout / 10, 1));
    bthread_timer_add(&_vote_timer, base::milliseconds_from_now(vote_timeout), on_vote_timer, this);
    RAFT_VLOG << "node " << _group_id << ":" << _server_id
        << " term " << _current_term << " start vote_timer";

    std::vector<PeerId> peers;
    _conf.second.peer_vector(&peers);
    _vote_ctx.set(peers.size());
    for (size_t i = 0; i < peers.size(); i++) {
        if (peers[i] == _server_id) {
            continue;
        }
        baidu::rpc::ChannelOptions options;
        options.connection_type = baidu::rpc::CONNECTION_TYPE_SINGLE;
        options.max_retry = 0;
        baidu::rpc::Channel channel;
        if (0 != channel.Init(peers[i].addr, &options)) {
            LOG(WARNING) << "channel init failed, addr " << peers[i].addr;
            continue;
        }

        RequestVoteRequest request;
        request.set_group_id(_group_id);
        request.set_server_id(_server_id.to_string());
        request.set_peer_id(peers[i].to_string());
        request.set_term(_current_term);
        request.set_last_log_term(last_log_term());
        request.set_last_log_index(_log_manager->last_log_index());

        OnRequestVoteRPCDone* done = new OnRequestVoteRPCDone(peers[i], _current_term, this);
        RaftService_Stub stub(&channel);
        stub.request_vote(&done->cntl, &request, &done->response, done);
    }

    _vote_ctx.grant(_server_id);
    //TODO: outof lock
    _stable_storage->set_term_and_votedfor(_current_term, _server_id);
    if (_vote_ctx.quorum()) {
        become_leader();
    }
}

// in lock
void NodeImpl::step_down(const int64_t term) {
    LOG(INFO) << "node " << _group_id << ":" << _server_id
        << " term " << _current_term << " stepdown from " << State2Str(_state)
        << " new_term " << term;

    if (_state == CANDIDATE) {
        RAFT_VLOG << "node " << _group_id << ":" << _server_id
            << " term " << _current_term << " stop vote_timer";
        int ret = bthread_timer_del(_vote_timer);
        if (0 == ret) {
            Release();
        } else {
            CHECK(ret == 1);
        }
    } else if (_state == LEADER) {
        RAFT_VLOG << "node " << _group_id << ":" << _server_id
            << " term " << _current_term << " stop stepdown_timer";
        int ret = bthread_timer_del(_stepdown_timer);
        if (0 == ret) {
            Release();
        } else {
            CHECK(ret == 1);
        }

        _commit_manager->clear_pending_applications();

        // stop disk thread, Release
        _log_manager->stop_disk_thread();
        Release();

        // signal fsm leader stop immediately
        _fsm_caller->on_leader_stop();
    }

    _state = FOLLOWER;
    _leader_id.reset();
    _current_term = term;
    _voted_id.reset();
    _conf_ctx.reset();
    //TODO: outof lock
    _stable_storage->set_term_and_votedfor(term, _voted_id);

    // no empty configuration, start election timer
    if (!_conf.second.empty() && _conf.second.contain(_server_id)) {
        AddRef();
        int64_t election_timeout = random_timeout(_options.election_timeout);
        bthread_timer_add(&_election_timer, base::milliseconds_from_now(election_timeout),
                          on_election_timer, this);
        RAFT_VLOG << "node " << _group_id << ":" << _server_id
            << " term " << _current_term << " start election_timer";
    }

    // stop stagging new node
    _replicator_group.stop_all();
}

// in lock
void NodeImpl::become_leader() {
    CHECK(_state == CANDIDATE);
    LOG(INFO) << "node " << _group_id << ":" << _server_id
        << " term " << _current_term << " become leader, and stop vote_timer";
    // cancel candidate vote timer
    int ret = bthread_timer_del(_vote_timer);
    if (0 == ret) {
        Release();
    } else {
        CHECK(ret == 1);
    }

    _state = LEADER;
    _leader_id = _server_id;

    // init disk thread, AddRef
    AddRef();
    _log_manager->start_disk_thread();

    // init replicator
    ReplicatorGroupOptions options;
    options.heartbeat_timeout_ms = std::max(_options.election_timeout / 10, 10);
    options.log_manager = _log_manager;
    options.commit_manager = _commit_manager;
    options.node = this;
    options.term = _current_term;
    options.snapshot_storage = _snapshot_storage;
    _replicator_group.init(NodeId(_group_id, _server_id), options);

    std::vector<PeerId> peers;
    _conf.second.peer_vector(&peers);
    for (size_t i = 0; i < peers.size(); i++) {
        if (peers[i] == _server_id) {
            continue;
        }

        RAFT_VLOG << "node " << _group_id << ":" << _server_id
            << " term " << _current_term
            << " add replicator " << peers[i];
        //TODO: check return code
        _replicator_group.add_replicator(peers[i]);
    }

    // init commit manager
    _commit_manager->reset_pending_index(_log_manager->last_log_index() + 1);

    // leader add peer first, as set_peer's configuration change log
    LogEntry* entry = new LogEntry;
    entry->term = _current_term;
    entry->type = ENTRY_TYPE_ADD_PEER;
    entry->peers = new std::vector<PeerId>;
    _conf.second.peer_vector(entry->peers);
    CHECK(entry->peers->size() > 0);

    //append(entry, NULL);
    append(entry, _fsm_caller->on_leader_start());

    AddRef();
    int64_t stepdown_timeout = _options.election_timeout;
    bthread_timer_add(&_stepdown_timer, base::milliseconds_from_now(stepdown_timeout),
                      on_stepdown_timer, this);
    RAFT_VLOG << "node " << _group_id << ":" << _server_id
        << " term " << _current_term << " start stepdown_timer";
}

LeaderStableClosure::LeaderStableClosure(NodeImpl* node, LogEntry* entry)
    : _node_id(node->node_id()), _node(node), _entry(entry) {
    _node->AddRef();
    _entry->AddRef();
}

LeaderStableClosure::~LeaderStableClosure() {
    _node->Release();
    _entry->Release();
}

void LeaderStableClosure::Run() {
    if (_err_code == 0) {
        _node->advance_commit_index(PeerId(), _entry->index);
    } else {
        LOG(ERROR) << "node " << _node_id << " append " << _entry->index << " failed";
    }
    // not free entry, FSMCaller will free it
    delete this;
}

void NodeImpl::advance_commit_index(const PeerId& peer_id, const int64_t log_index) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);
    if (peer_id.is_empty()) {
        // leader thread
        _commit_manager->set_stable_at_peer_reentrant(log_index, _server_id);
    } else {
        // peer thread
        _commit_manager->set_stable_at_peer_reentrant(log_index, peer_id);
    }

    // commit_manager check quorum ok, will call fsm_caller
}

void NodeImpl::append(LogEntry* entry, Closure* done) {
    // configuration change use new peer set
    std::vector<PeerId> old_peers;
    if (entry->type != ENTRY_TYPE_ADD_PEER && entry->type != ENTRY_TYPE_REMOVE_PEER) {
        _commit_manager->append_pending_application(_conf.second, done);
    } else  {
        _conf.second.peer_vector(&old_peers);
        _commit_manager->append_pending_application(Configuration(*(entry->peers)), done);
    }
    _log_manager->append_entry(entry, new LeaderStableClosure(this, entry));
    if (_log_manager->check_and_set_configuration(_conf)) {
        _conf_ctx.set(old_peers);
    }
}

int NodeImpl::append(const std::vector<LogEntry*>& entries) {
    if (entries.size() == 0) {
        return 0;
    }
    int ret = _log_manager->append_entries(entries);
    if (ret == 0) {
        _log_manager->check_and_set_configuration(_conf);
    } else {
        LogEntry* first_entry = entries[0];
        LogEntry* last_entry = entries[entries.size() - 1];
        LOG(ERROR) << "node " << _group_id << ":" << _server_id
            << " append " << first_entry->index << " -> " << last_entry->index << " failed";
    }

    return ret;
}

int NodeImpl::handle_request_vote_request(const RequestVoteRequest* request,
                                          RequestVoteResponse* response) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    int64_t last_log_index = _log_manager->last_log_index();
    int64_t last_log_term = this->last_log_term();
    bool log_is_ok = (request->last_log_term() > last_log_term ||
                      (request->last_log_term() == last_log_term &&
                       request->last_log_index() >= last_log_index));
    PeerId candidate_id;
    if (0 != candidate_id.parse(request->server_id())) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id
            << " received RequestVote from " << request->server_id()
            << " server_id bad format";
        return EINVAL;
    }

    do {
        // check leader to tolerate network partitioning:
        //     1. leader always reject RequstVote
        //     2. follower reject RequestVote before change to candidate
        if (!_leader_id.is_empty()) {
            LOG(WARNING) << "node " << _group_id << ":" << _server_id
                << " reject RequestVote from " << request->server_id()
                << " in term " << request->term()
                << " current_term " << _current_term
                << " current_leader " << _leader_id;
            break;
        }

        // check term
        if (request->term() >= _current_term) {
            LOG(INFO) << "node " << _group_id << ":" << _server_id
                << " received RequestVote from " << request->server_id()
                << " in term " << request->term()
                << " current_term " << _current_term;
            // incress current term, change state to follower
            if (request->term() > _current_term) {
                step_down(request->term());
            }
        } else {
            // ignore older term
            LOG(INFO) << "node " << _group_id << ":" << _server_id
                << " ignore RequestVote from " << request->server_id()
                << " in term " << request->term()
                << " current_term " << _current_term;
            break;
        }

        // save
        if (log_is_ok && _voted_id.is_empty()) {
            _voted_id = candidate_id;
            //TODO: outof lock
            _stable_storage->set_votedfor(candidate_id);
        }
    } while (0);

    response->set_term(_current_term);
    response->set_granted(request->term() == _current_term && _voted_id == candidate_id);
    return 0;
}

int NodeImpl::handle_append_entries_request(base::IOBuf& data_buf,
                                            const AppendEntriesRequest* request,
                                            AppendEntriesResponse* response) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    PeerId server_id;
    if (0 != server_id.parse(request->server_id())) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id
            << " received RequestVote from " << request->server_id()
            << " server_id bad format";
        return EINVAL;
    }

    bool success = false;
    do {
        // check stale term
        if (request->term() < _current_term) {
            LOG(WARNING) << "node " << _group_id << ":" << _server_id
                << " ignore stale AppendEntries from " << request->server_id()
                << " in term " << request->term()
                << " current_term " << _current_term;
            break;
        }

        // check term and state to step down
        if (request->term() > _current_term || _state != FOLLOWER) {
            step_down(request->term());
        }

        // save current leader
        if (_leader_id.is_empty()) {
            _leader_id = server_id;
        }

        // not loading snapshot, install snapshot
        CHECK(NULL == _loading_snapshot_meta);

        // check prev log gap
        if (request->prev_log_index() > _log_manager->last_log_index()) {
            LOG(WARNING) << "node " << _group_id << ":" << _server_id
                << " reject index_gapped AppendEntries from " << request->server_id()
                << " in term " << request->term()
                << " prev_log_index " << request->prev_log_index()
                << " last_log_index " << _log_manager->last_log_index();
            break;
        }

        // check prev log term
        if (request->prev_log_index() >= _log_manager->first_log_index()) {
            int64_t local_term = _log_manager->get_term(request->prev_log_index());
            if (local_term != request->prev_log_term()) {
                LOG(WARNING) << "node " << _group_id << ":" << _server_id
                    << " reject term_unmatched AppendEntries from " << request->server_id()
                    << " in term " << request->term()
                    << " prev_log_index " << request->prev_log_index()
                    << " prev_log_term " << request->prev_log_term()
                    << " prev_log_term_local " << local_term;
                break;
            }
        }

        success = true;

        std::vector<LogEntry*> entries;
        int64_t index = request->prev_log_index();
        for (int i = 0; i < request->entries_size(); i++) {
            index++;

            const EntryMeta& entry = request->entries(i);

            if (index < _log_manager->first_log_index()) {
                // log maybe discard after snapshot, skip retry AppendEntries rpc
                continue;
            }
            // mostly index == _log_manager->last_log_index() + 1
            if (_log_manager->last_log_index() >= index) {
                if (_log_manager->get_term(index) == entry.term()) {
                    // check same index entry's term when duplicated rpc
                    continue;
                }

                int64_t last_index_kept = index - 1;
                LOG(WARNING) << "node " << _group_id << ":" << _server_id
                    << " term " << _current_term
                    << " truncate from " << _log_manager->last_log_index()
                    << " to " << last_index_kept;

                _log_manager->truncate_suffix(last_index_kept);
                // truncate configuration
                _log_manager->check_and_set_configuration(_conf);
            }

            if (entry.type() != ENTRY_TYPE_UNKNOWN) {
                LogEntry* log_entry = new LogEntry();
                log_entry->term = entry.term();
                log_entry->type = (EntryType)entry.type();
                if (entry.peers_size() > 0) {
                    log_entry->peers = new std::vector<PeerId>;
                    for (int i = 0; i < entry.peers_size(); i++) {
                        log_entry->peers->push_back(entry.peers(i));
                    }
                    CHECK((log_entry->type == ENTRY_TYPE_ADD_PEER
                            || log_entry->type == ENTRY_TYPE_REMOVE_PEER));
                } else {
                    CHECK_NE(entry.type(), ENTRY_TYPE_ADD_PEER);
                }

                if (entry.has_data_len()) {
                    int len = entry.data_len();
                    data_buf.cutn(&log_entry->data, len);
                }

                entries.push_back(log_entry);
            }
        }

        RAFT_VLOG << "node " << _group_id << ":" << _server_id
            << " received " << (entries.size() > 0 ? "AppendEntries" : "Heartbeat")
            << " from " << request->server_id()
            << " in term " << request->term()
            << " prev_index " << request->prev_log_index()
            << " prev_term " << request->prev_log_term()
            << " count " << entries.size()
            << " current_term " << _current_term;

        //TODO2: outof lock
        if (0 != append(entries)) {
            // free entry
            for (size_t i = 0; i < entries.size(); i++) {
                LogEntry* entry = entries[i];
                entry->Release();
            }
        }
    } while (0);

    response->set_term(_current_term);
    response->set_success(success);
    response->set_last_log_index(_log_manager->last_log_index());
    if (success) {
        // commit manager call fsmcaller
        _commit_manager->set_last_committed_index(request->committed_index());
        _last_leader_timestamp = base::monotonic_time_ms();
    }
    return 0;
}

InstallSnapshotDone::InstallSnapshotDone(NodeImpl* node, SnapshotStorage* snapshot_storage,
                                         baidu::rpc::Controller* controller,
                                         const InstallSnapshotRequest* request,
                                         InstallSnapshotResponse* response,
                                         google::protobuf::Closure* done)
    : _node(node), _snapshot_storage(snapshot_storage), _reader(NULL),
    _controller(controller), _request(request), _response(response), _done(done) {
    _node->AddRef();
}

InstallSnapshotDone::~InstallSnapshotDone() {
    _node->Release();
}

SnapshotReader* InstallSnapshotDone::start() {
    _reader = _snapshot_storage->open();
    return _reader;
}

void InstallSnapshotDone::Run() {
    if (_err_code == 0) {
        _node->on_snapshot_load_done();
        _response->set_success(true);
    } else {
        _response->set_success(false);
    }

    if (_reader) {
        _snapshot_storage->close(_reader);
    }
    // RPC done, just transfer response
    _done->Run();
    delete this;
}

int NodeImpl::handle_install_snapshot_request(baidu::rpc::Controller* controller,
                                              const InstallSnapshotRequest* request,
                                              InstallSnapshotResponse* response,
                                              google::protobuf::Closure* done) {
    int ret = 0;

    // some check
    {
        std::lock_guard<bthread_mutex_t> guard(_mutex);

        PeerId server_id;
        if (0 != server_id.parse(request->server_id())) {
            LOG(WARNING) << "node " << _group_id << ":" << _server_id
                << " received InstallSnapshotRequest from " << request->server_id()
                << " server_id bad format";
            return EINVAL;
        }

        response->set_success(false);
        response->set_term(_current_term);

        // check loading snapshot?
        if (_loading_snapshot_meta) {
            LOG(WARNING) << "node " << _group_id << ":" << _server_id
                << " received InstallSnapshotRequest from " << request->server_id()
                << " install snapshot running";
            return EAGAIN;
        }

        // check staled term
        if (request->term() < _current_term) {
            LOG(WARNING) << "node " << _group_id << ":" << _server_id << " term " << _current_term
                << " received staled InstallSnapshotRequest term " << request->term();
            // TODO: don run here
            done->Run();
            return 0;
        }

        // check term and state
        if (request->term() > _current_term || _state != FOLLOWER) {
            response->set_term(_current_term);
            step_down(_current_term);
        }

        // save current leader
        if (_leader_id.is_empty()) {
            _leader_id = server_id;
        }

        // retry InstallSnapshot
        if (request->last_included_log_index() == _last_snapshot_index &&
            request->last_included_log_term() == _last_snapshot_term) {
            LOG(WARNING) << "node " << _group_id << ":" << _server_id << " term " << _current_term
                << " received retry InstallSnapshotRequest from " << request->server_id();
            response->set_success(true);
            done->Run();
            return 0;
        }

        // some check, imposible case
        CHECK(request->last_included_log_index() > _last_snapshot_index);
        CHECK(request->last_included_log_index() > _log_manager->last_log_index());

        // start thread do fetch and load snapshot
        SnapshotMeta* meta = new SnapshotMeta();
        meta->last_included_index = request->last_included_log_index();
        meta->last_included_term = request->last_included_log_term();
        for (int i = 0; i < request->peers_size(); i++) {
            PeerId peer;
            if (0 != peer.parse(request->peers(i))) {
                LOG(WARNING) << "node " << _group_id << ":" << _server_id
                    << " received InstallSnapshotRequest from " << request->server_id()
                    << " peers bad format";
                delete meta;
                return EINVAL;
            }
            meta->last_configuration.add_peer(peer);
        }

        _loading_snapshot_meta = meta;
    }

    // copy snapshot
    SnapshotWriter* writer = NULL;
    do {
        CHECK(_snapshot_storage);

        writer = _snapshot_storage->create(*_loading_snapshot_meta);
        if (NULL == writer) {
            ret = EINVAL;
            break;
        }

        ret = writer->copy(request->uri());
        if (0 != ret) {
            break;
        }

        ret = writer->save_meta();
        if (0 != ret) {
            break;
        }
    } while (0);
    if (NULL != writer) {
        _snapshot_storage->close(writer);
    }
    if (0 != ret) {
        LOG(WARNING) << "node " << _group_id << ":" << _server_id
            << " term " << _current_term << " snapshot save failed, uri " << request->uri();
        return ret;
    }

    // fsm load snapshot
    {
        std::lock_guard<bthread_mutex_t> guard(_mutex);

        //TODO:
        // call fsm_caller on_install_snapshot, when finished run on_snapshot_load_done
        InstallSnapshotDone* install_snapshot_done =
            new InstallSnapshotDone(this, _snapshot_storage, controller, request, response, done);
        _fsm_caller->on_snapshot_load(install_snapshot_done);
    }

    return 0;
}

int NodeImpl::increase_term_to(int64_t new_term) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);
    if (new_term <= _current_term) {
        return EINVAL;
    }
    step_down(new_term);
    return 0;
}

NodeManager::NodeManager() {
    bthread_mutex_init(&_mutex, NULL);
}

NodeManager::~NodeManager() {
    bthread_mutex_destroy(&_mutex);
}

int NodeManager::init(const char* ip_str, int start_port, int end_port) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);
    if (_address.ip != base::IP_ANY) {
        LOG(ERROR) << "Raft has be inited";
        return EINVAL;
    }

    baidu::rpc::ServerOptions server_options;
    if (0 != _server.AddService(new FileServiceImpl, baidu::rpc::SERVER_OWNS_SERVICE)) {
        LOG(ERROR) << "Add File Service Failed.";
        return EINVAL;
    }
    if (0 != _server.AddService(&_service_impl, baidu::rpc::SERVER_DOESNT_OWN_SERVICE)) {
        LOG(ERROR) << "Add Raft Service Failed.";
        return EINVAL;
    }
    if (0 != _server.Start(ip_str, start_port, end_port, &server_options)) {
        LOG(ERROR) << "Start Raft Server Failed.";
        return EINVAL;
    }

    _address = _server.listen_address();
    if (_address.ip == base::IP_ANY) {
        _address.ip = base::get_host_ip();
    }
    LOG(WARNING) << "start raft server " << _address;
    return 0;
}

base::EndPoint NodeManager::address() {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    return _address;
}

bool NodeManager::add(NodeImpl* node) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    NodeId node_id = node->node_id();
    NodeMap::iterator it = _nodes.find(node_id);
    bool success = true;
    if (it == _nodes.end()) {
        _nodes.insert(std::pair<NodeId, NodeImpl*>(node_id, node));
    } else {
        success = false;
    }
    return success;
}

void NodeManager::remove(NodeImpl* node) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    _nodes.erase(node->node_id());
}

scoped_refptr<NodeImpl> NodeManager::get(const GroupId& group_id, const PeerId& peer_id) {
    std::lock_guard<bthread_mutex_t> guard(_mutex);

    NodeMap::iterator it = _nodes.find(NodeId(group_id, peer_id));
    if (it != _nodes.end()) {
        return make_scoped_refptr(it->second);
    } else {
        return scoped_refptr<NodeImpl>();
    }
}

}

