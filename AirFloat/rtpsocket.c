//
//  rtpsocket.c
//  AirFloat
//
//  Copyright (c) 2013, Kristian Trenskow All rights reserved.
//
//  Redistribution and use in source and binary forms, with or
//  without modification, are permitted provided that the following
//  conditions are met:
//
//  Redistributions of source code must retain the above copyright
//  notice, this list of conditions and the following disclaimer.
//  Redistributions in binary form must reproduce the above
//  copyright notice, this list of conditions and the following
//  disclaimer in the documentation and/or other materials provided
//  with the distribution. THIS SOFTWARE IS PROVIDED BY THE
//  COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
//  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
//  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
//  OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
//  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
//  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
//  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "obj.h"

#include "rtpsocket.h"

struct rtp_socket_info_t {
    socket_p socket;
    bool is_data_socket;
};

struct rtp_socket_t {
    char* name;
    struct sockaddr* allowed_remote_end_point;
    struct rtp_socket_info_t** sockets;
    uint32_t sockets_count;
    mutex_p mutex;
    struct {
        rtp_socket_data_received_callback data_received;
        struct {
            void* data_received;
        } ctx;
    } callbacks;
};

struct rtp_socket_t* rtp_socket_create(const char* name, struct sockaddr* allowed_remote_end_point) {
    
    struct rtp_socket_t* rs = (struct rtp_socket_t*)obj_create(sizeof(struct rtp_socket_t));
    
    if (allowed_remote_end_point != NULL)
        rs->allowed_remote_end_point = sockaddr_copy(allowed_remote_end_point);
    
    if (name) {
        rs->name = (char*)malloc(strlen(name) + 1);
        strcpy(rs->name, name);
    }
    
    rs->mutex = mutex_create_recursive();
    
    return rs;
    
}

void _rtp_socket_destroy(void* obj) {
    
    struct rtp_socket_t* rs = (struct rtp_socket_t*)obj;
    
    mutex_lock(rs->mutex);
    
    for (uint32_t i = 0 ; i < rs->sockets_count ; i++) {
        socket_close(rs->sockets[i]->socket);
        socket_release(rs->sockets[i]->socket);
    }
    
    free(rs->sockets);
    
    rs->allowed_remote_end_point = sockaddr_release(rs->allowed_remote_end_point);
    
    if (rs->name)
        free(rs->name);
    
    mutex_unlock(rs->mutex);
    mutex_release(rs->mutex);
    
}

struct rtp_socket_t* rtp_socket_retain(struct rtp_socket_t* rs) {
    
    return obj_retain(rs);
    
}

struct rtp_socket_t* rtp_socket_release(struct rtp_socket_t* rs) {
    
    return obj_release(rs, _rtp_socket_destroy);
    
}

void _rtp_socket_socket_closed_callback(socket_p socket, void* ctx) {
    
    struct rtp_socket_t* rs = (struct rtp_socket_t*)ctx;
    
    mutex_lock(rs->mutex);
    
    for (uint32_t i = 0 ; i < rs->sockets_count ; i++)
        if (rs->sockets[i]->socket == socket) {
            
            socket_release(rs->sockets[i]->socket);
            
            free(rs->sockets[i]);
            
            for (uint32_t a = i + 1 ; a < rs->sockets_count ; a++)
                rs->sockets[a - 1] = rs->sockets[a];
            
            rs->sockets_count--;
            
            break;
            
        }
    
    mutex_unlock(rs->mutex);
    
}

ssize_t _rtp_socket_socket_receive_callback(socket_p socket, const void* data, size_t data_size, struct sockaddr* remote_end_point, void* ctx) {
    
    struct rtp_socket_t* rs = (struct rtp_socket_t*)ctx;
    
    size_t used = data_size;
    
    if (sockaddr_equals_host(remote_end_point, rs->allowed_remote_end_point) && rs->callbacks.data_received != NULL)
        used = rs->callbacks.data_received(rs, socket, (const char*)data, data_size, rs->callbacks.ctx.data_received);
    
    return used;
    
}

struct rtp_socket_info_t* _rtp_socket_add_socket(struct rtp_socket_t* rs, socket_p socket, bool is_data_socket) {
    
    struct rtp_socket_info_t* info = (struct rtp_socket_info_t*)malloc(sizeof(struct rtp_socket_info_t));
    bzero(info, sizeof(struct rtp_socket_info_t));
    info->socket = socket_retain(socket);
    info->is_data_socket = is_data_socket;
        
    if (is_data_socket)
        socket_set_receive_callback(socket, _rtp_socket_socket_receive_callback, rs);
    
    socket_set_closed_callback(socket, _rtp_socket_socket_closed_callback, rs);
    
    mutex_lock(rs->mutex);
    
    rs->sockets = (struct rtp_socket_info_t**)realloc(rs->sockets, sizeof(struct rtp_socket_info_t*) * (rs->sockets_count + 1));
    rs->sockets[rs->sockets_count] = info;
    rs->sockets_count++;
    
    mutex_unlock(rs->mutex);
    
    return info;
    
}

bool _rtp_socket_accept_callback(socket_p socket, socket_p new_socket, void* ctx) {
    
    struct rtp_socket_t* rs = (struct rtp_socket_t*)ctx;
    
    if (new_socket != NULL) {
        
        if (rs->allowed_remote_end_point == NULL || sockaddr_equals_host(socket_get_remote_end_point(new_socket), rs->allowed_remote_end_point)) {
            _rtp_socket_add_socket(rs, new_socket, true);
            return true;
        } else {
            socket_close(new_socket);
            socket_release(new_socket);
        }
        
    }
    
    return false;
    
}


bool rtp_socket_setup(struct rtp_socket_t* rs, struct sockaddr* local_end_point) {
    
    socket_p udp_socket = socket_create(true);
    socket_p tcp_socket = socket_create(false);
    
    socket_set_name(udp_socket, "RTP UDP Socket");
    socket_set_name(tcp_socket, "RTP TCP Listen Socket");
    
    if (socket_bind(udp_socket, local_end_point) && socket_bind(tcp_socket, local_end_point)) {
        _rtp_socket_add_socket(rs, udp_socket, true);
        socket_set_receive_callback(udp_socket, _rtp_socket_socket_receive_callback, rs);
        _rtp_socket_add_socket(rs, tcp_socket, false);
        socket_set_accept_callback(tcp_socket, _rtp_socket_accept_callback, rs);
        return true;
    }
    
    socket_release(udp_socket);
    socket_release(tcp_socket);
    
    return false;
    
}

void rtp_socket_set_data_received_callback(struct rtp_socket_t* rs, rtp_socket_data_received_callback callback, void* ctx) {
    
    rs->callbacks.data_received = callback;
    rs->callbacks.ctx.data_received = ctx;
    
}

void rtp_socket_send_to(struct rtp_socket_t* rs, struct sockaddr* dst, const void* buffer, uint32_t size) {
    
    for (uint32_t i = 0 ; i < rs->sockets_count ; i++)
        if (rs->sockets[i]->is_data_socket)
            socket_send_to(rs->sockets[i]->socket, dst, buffer, size);
    
}

uint16_t rtp_socket_get_local_port(rtp_socket_p rs) {
    
    for (uint32_t i = 0 ; i < rs->sockets_count ; i++)
        if (!rs->sockets[i]->is_data_socket)
            return sockaddr_get_port(socket_get_local_end_point(rs->sockets[i]->socket));
    
    return 0;
    
}
