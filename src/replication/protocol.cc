#include "replication/protocol.hpp"

using boost::scoped_ptr;

namespace replication {

class protocol_exc_t : public std::exception {
public:
    protocol_exc_t(const char *msg) : msg_(msg) { }
    const char *what() throw() { return msg_; }
private:
    const char *msg_;
};

// The 16 bytes conveniently include a \0 at the end.
// 13 is the length of the text.
const char STANDARD_HELLO_MAGIC[16] = "13rethinkdbrepl";

bool valid_role(uint32_t val) {
    return val == master || val == new_slave || val == slave;
}

void do_parse_hello_message(tcp_conn_t *conn, message_callback_t *receiver) {
    net_hello_t buf;
    conn->read(&buf, sizeof(buf));

    rassert(16 == sizeof(STANDARD_HELLO_MAGIC));
    if (0 != memcmp(buf.hello_magic, STANDARD_HELLO_MAGIC, sizeof(STANDARD_HELLO_MAGIC))) {
        throw protocol_exc_t("bad hello magic");  // TODO details
    }

    if (buf.replication_protocol_version != 1) {
        throw protocol_exc_t("bad protocol version");  // TODO details
    }

    if (!valid_role(buf.role)) {
        throw protocol_exc_t("bad protocol role");  // TODO details
    }

    rassert(32 == sizeof(buf.informal_name));
    scoped_ptr<hello_message_t> msg(new hello_message_t(role_t(buf.role), buf.database_magic, buf.informal_name, buf.informal_name + sizeof(buf.informal_name)));
    receiver->hello(msg);
}

template <class struct_type>
bool fits(const char *buffer, size_t size) {
    return size >= sizeof(struct_type);
}

template <>
bool fits<net_small_set_t>(const char *buffer, size_t size) {
    const net_small_set_t *p = reinterpret_cast<const net_small_set_t *>(buffer);

    return sizeof(net_small_set_t) < size && leaf_pair_fits(p->leaf_pair(), size - sizeof(net_small_set_t));
}

template <>
bool fits<net_small_append_prepend_t>(const char *buffer, size_t size) {
    const net_small_append_prepend_t *p = reinterpret_cast<const net_small_append_prepend_t *>(buffer);

    return sizeof(net_small_append_prepend_t) <= size && sizeof(net_small_append_prepend_t) + p->size <= size;
}

template <class struct_type>
ssize_t objsize(const struct_type *buffer) {
    return sizeof(struct_type);
}

template <>
ssize_t objsize<net_small_set_t>(const net_small_set_t *buffer) {
    return sizeof(net_small_set_t) + leaf::pair_size(buffer->leaf_pair());
}

template <>
ssize_t objsize<net_small_append_prepend_t>(const net_small_append_prepend_t *buffer) {
    return sizeof(net_small_append_prepend_t) + buffer->size;
}

template <class message_type>
ssize_t try_parsing(message_callback_t *receiver, const char *buffer, size_t size) {
    typedef typename message_type::net_struct_type net_struct_type;
    if (fits<net_struct_type>(buffer, size)) {
        const net_struct_type *p = reinterpret_cast<const net_struct_type *>(buffer);

        scoped_ptr<message_type> msg(new message_type(p));

        receiver->send(msg);
        return objsize<net_struct_type>(p);
    }
    return -1;
}

template <class message_type>
bool parse_and_pop(tcp_conn_t *conn, message_callback_t *receiver, const char *buffer, size_t size) {
    ssize_t sz = try_parsing<message_type>(receiver, buffer, size);
    if (sz == -1) {
        return false;
    } else {
        conn->pop(sz);
        return true;
    }
}

template <class message_type>
bool parse_and_stream(tcp_conn_t *conn, message_callback_t *receiver, tcp_conn_t::bufslice sl, std::vector<value_stream_t *>& streams) {
    typedef typename message_type::first_struct_type first_struct_type;
    /*    if (part_before_stream_fits<first_struct_type>(sl)) {
        const first_struct_type *p = reinterpret_cast<const first_struct_type *>(sl.buf);
        value_stream_t *stream = new value_stream_t();
        size_t size_before = size_before_stream<first_struct_size>(sl);
        stream.open_space_for_writing(p->op_header

    }
    */
    return false;
}

void do_parse_normal_message(tcp_conn_t *conn, message_callback_t *receiver, std::vector<value_stream_t *>& streams) {

    for (;;) {
        tcp_conn_t::bufslice sl = conn->peek();
        if (sl.len >= sizeof(net_header_t)) {
            const char *buffer = reinterpret_cast<const char *>(sl.buf);
            const net_header_t *hdr = reinterpret_cast<const net_header_t *>(sl.buf);

            int multipart_aspect = hdr->message_multipart_aspect;

            if (multipart_aspect == SMALL) {

                switch (hdr->msgcode) {
                case BACKFILL: { if (parse_and_pop<backfill_message_t>(conn, receiver, buffer, sl.len)) return; } break;
                case ANNOUNCE: { if (parse_and_pop<announce_message_t>(conn, receiver, buffer, sl.len)) return; } break;
                case NOP: { if (parse_and_pop<nop_message_t>(conn, receiver, buffer, sl.len)) return; } break;
                case ACK: { if (parse_and_pop<ack_message_t>(conn, receiver, buffer, sl.len)) return; } break;
                case SHUTTING_DOWN: { if (parse_and_pop<shutting_down_message_t>(conn, receiver, buffer, sl.len)) return; } break;
                case GOODBYE: { if (parse_and_pop<goodbye_message_t>(conn, receiver, buffer, sl.len)) return; } break;
                case SET: { if (parse_and_pop<set_message_t>(conn, receiver, buffer, sl.len)) return; } break;
                case APPEND: { if (parse_and_pop<append_message_t>(conn, receiver, buffer, sl.len)) return; } break;
                case PREPEND: { if (parse_and_pop<prepend_message_t>(conn, receiver, buffer, sl.len)) return; } break;
                default: { throw protocol_exc_t("invalid message code"); }
                }

            } else if (multipart_aspect == FIRST) {

                if (sl.len >= sizeof(net_large_operation_first_t)) {
                    const net_large_operation_first_t *firstheader
                        = reinterpret_cast<const net_large_operation_first_t *>(buffer);

                    switch (firstheader->header.header.msgcode) {
                    case SET: { if (parse_and_stream<set_message_t>(conn, receiver, sl, streams)) return; } break;
                    case APPEND: { if (parse_and_stream<append_message_t>(conn, receiver, sl, streams)) return; } break;
                    case PREPEND: { if (parse_and_stream<prepend_message_t>(conn, receiver, sl, streams)) return; } break;
                    default: { throw protocol_exc_t("invalid message code on FIRST message"); }
                    }

                }


            } else {
                throw protocol_exc_t("invalid multipart_aspect (small messages only supported)");

            }
        }

        conn->read_more_buffered();
    }
}


void do_parse_messages(tcp_conn_t *conn, message_callback_t *receiver) {
    do_parse_hello_message(conn, receiver);

    std::vector<value_stream_t *> placeholder;
    for (;;) {
        do_parse_normal_message(conn, receiver, placeholder);
    }
}

void parse_messages(tcp_conn_t *conn, message_callback_t *receiver) {
    try {
        do_parse_messages(conn, receiver);
    }
    catch (tcp_conn_t::read_closed_exc_t& e) {
        receiver->conn_closed();
    }
}

}  // namespace replication