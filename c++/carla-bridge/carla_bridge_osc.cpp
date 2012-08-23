/*
 * Carla Plugin bridge code
 * Copyright (C) 2012 Filipe Coelho <falktx@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the COPYING file
 */

#include "carla_bridge_osc.h"
#include "carla_bridge_client.h"
#include "carla_midi.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

CARLA_BRIDGE_START_NAMESPACE

void osc_error_handler(const int num, const char* const msg, const char* const path)
{
    qCritical("osc_error_handler(%i, \"%s\", \"%s\")", num, msg, path);
}

// -----------------------------------------------------------------------

CarlaBridgeOsc::CarlaBridgeOsc(CarlaBridgeClient* const client_, const char* const name) :
    client(client_)
{
    qDebug("CarlaBridgeOsc::CarlaBridgeOsc(%p, \"%s\")", client_, name);
    Q_ASSERT(client_);
    Q_ASSERT(name);

    m_serverPath = nullptr;
    m_serverThread = nullptr;
    m_controlData.path = nullptr;
    m_controlData.source = nullptr; // unused
    m_controlData.target = nullptr;

    m_name = strdup(name);
    m_nameSize = strlen(name);
}

CarlaBridgeOsc::~CarlaBridgeOsc()
{
    qDebug("CarlaBridgeOsc::~CarlaBridgeOsc()");

    free(m_name);
}

bool CarlaBridgeOsc::init(const char* const url)
{
    qDebug("CarlaBridgeOsc::init(\"%s\")", url);
    Q_ASSERT(! m_serverThread);
    Q_ASSERT(url);

    char* host = lo_url_get_hostname(url);
    char* port = lo_url_get_port(url);

    m_controlData.path   = lo_url_get_path(url);
    m_controlData.target = lo_address_new(host, port);

    free(host);
    free(port);

    if (! m_controlData.path)
    {
        qWarning("CarlaBridgeOsc::init(\"%s\") - failed to init OSC", url);
        return false;
    }

    // create new OSC thread
    m_serverThread = lo_server_thread_new(nullptr, osc_error_handler);

    // get our full OSC server path
    char* const threadPath = lo_server_thread_get_url(m_serverThread);
    m_serverPath = strdup(QString("%1%2").arg(threadPath).arg(m_name).toUtf8().constData());
    free(threadPath);

    // register message handler and start OSC thread
    lo_server_thread_add_method(m_serverThread, nullptr, nullptr, osc_message_handler, this);
    lo_server_thread_start(m_serverThread);

    return true;
}

void CarlaBridgeOsc::close()
{
    qDebug("CarlaBridgeOsc::close()");
    Q_ASSERT(m_serverThread);

    osc_clear_data(&m_controlData);

    lo_server_thread_stop(m_serverThread);
    lo_server_thread_del_method(m_serverThread, nullptr, nullptr);
    lo_server_thread_free(m_serverThread);

    free((void*)m_serverPath);
    m_serverPath = nullptr;
}

int CarlaBridgeOsc::handleMessage(const char* const path, const int argc, const lo_arg* const* const argv, const char* const types, const lo_message msg)
{
    qDebug("CarlaBridgeOsc::handleMessage(\"%s\", %i, %p, \"%s\", %p)", path, argc, argv, types, msg);
    Q_ASSERT(m_serverThread);
    Q_ASSERT(path);

    // Check if message is for this client
    if (strlen(path) <= m_nameSize || strncmp(path+1, m_name, m_nameSize) != 0)
    {
        qWarning("CarlaBridgeOsc::handleMessage() - message not for this client -> '%s' != '/%s/'", path, m_name);
        return 1;
    }

    char method[32] = { 0 };
    memcpy(method, path + (m_nameSize + 1), 32);

    // Common OSC methods
    if (strcmp(method, "/configure") == 0)
        return handle_configure(argc, argv, types);
    if (strcmp(method, "/control") == 0)
        return handle_control(argc, argv, types);
    if (strcmp(method, "/program") == 0)
        return handle_program(argc, argv, types);
    if (strcmp(method, "/midi_program") == 0)
        return handle_midi_program(argc, argv, types);
    if (strcmp(method, "/midi") == 0)
        return handle_midi(argc, argv, types);
    if (strcmp(method, "/show") == 0)
        return handle_show();
    if (strcmp(method, "/hide") == 0)
        return handle_hide();
    if (strcmp(method, "/quit") == 0)
        return handle_quit();

#ifdef BRIDGE_LV2
    if (strcmp(method, "/lv2_atom_transfer") == 0)
        return handle_lv2_atom_transfer(argc, argv, types);
    if (strcmp(method, "/lv2_event_transfer") == 0)
        return handle_lv2_event_transfer(argc, argv, types);
#endif

#if 0
    else if (strcmp(method, "set_parameter_midi_channel") == 0)
        return osc_set_parameter_midi_channel_handler(argv);
    else if (strcmp(method, "set_parameter_midi_cc") == 0)
        return osc_set_parameter_midi_channel_handler(argv);
#endif

    qWarning("CarlaBridgeOsc::handleMessage(\"%s\", ...) - Got unsupported OSC method '%s'", path, method);
    return 1;
}

int CarlaBridgeOsc::handle_configure(CARLA_BRIDGE_OSC_HANDLE_ARGS)
{
    qDebug("CarlaOsc::handle_configure()");
    CARLA_BRIDGE_OSC_CHECK_OSC_TYPES(2, "ss");

    if (! client)
        return 1;

#ifdef BUILD_BRIDGE_PLUGIN
    const char* const key   = (const char*)&argv[0]->s;
    const char* const value = (const char*)&argv[1]->s;

    if (strcmp(key, CARLA_BRIDGE_MSG_SAVE_NOW) == 0)
    {
        client->quequeMessage(BRIDGE_MESSAGE_SAVE_NOW, 0, 0, 0.0);
    }
    else if (strcmp(key, CARLA_BRIDGE_MSG_SET_CHUNK) == 0)
    {
        client->setChunkData(value);
    }
    else if (strcmp(key, CARLA_BRIDGE_MSG_SET_CUSTOM) == 0)
    {
        QStringList vList = QString(value).split("·", QString::KeepEmptyParts);

        if (vList.size() == 3)
        {
            const char* const cType  = vList.at(0).toUtf8().constData();
            const char* const cKey   = vList.at(1).toUtf8().constData();
            const char* const cValue = vList.at(2).toUtf8().constData();

            client->set_custom_data(cType, cKey, cValue);
        }
    }
#else
    Q_UNUSED(argv);
#endif

    return 0;
}

int CarlaBridgeOsc::handle_control(CARLA_BRIDGE_OSC_HANDLE_ARGS)
{
    qDebug("CarlaOsc::handle_control()");
    CARLA_BRIDGE_OSC_CHECK_OSC_TYPES(2, "if");

    if (! client)
        return 1;

    const int32_t index = argv[0]->i;
    const float   value = argv[1]->f;
    client->quequeMessage(MESSAGE_PARAMETER, index, 0, value);

    return 0;
}

int CarlaBridgeOsc::handle_program(CARLA_BRIDGE_OSC_HANDLE_ARGS)
{
    qDebug("CarlaOsc::handle_program()");
    CARLA_BRIDGE_OSC_CHECK_OSC_TYPES(1, "i");

    if (! client)
        return 1;

    const int32_t index = argv[0]->i;
    client->quequeMessage(MESSAGE_PROGRAM, index, 0, 0.0);

    return 0;
}

int CarlaBridgeOsc::handle_midi_program(CARLA_BRIDGE_OSC_HANDLE_ARGS)
{
    qDebug("CarlaOsc::handle_midi_program()");
    CARLA_BRIDGE_OSC_CHECK_OSC_TYPES(2, "ii");

    if (! client)
        return 1;

    const int32_t bank    = argv[0]->i;
    const int32_t program = argv[1]->i;
    client->quequeMessage(MESSAGE_MIDI_PROGRAM, bank, program, 0.0);

    return 0;
}

int CarlaBridgeOsc::handle_midi(CARLA_BRIDGE_OSC_HANDLE_ARGS)
{
    qDebug("CarlaOsc::handle_midi()");
    CARLA_BRIDGE_OSC_CHECK_OSC_TYPES(1, "m");

    if (! client)
        return 1;

    const uint8_t* const mdata    = argv[0]->m;
    const uint8_t         data[4] = { mdata[0], mdata[1], mdata[2], mdata[3] };

    uint8_t status = data[1];

    // Fix bad note-off
    if (MIDI_IS_STATUS_NOTE_ON(status) && data[3] == 0)
        status -= 0x10;

    if (MIDI_IS_STATUS_NOTE_OFF(status))
    {
        uint8_t note = data[2];
        client->quequeMessage(MESSAGE_NOTE_OFF, note, 0, 0.0);
    }
    else if (MIDI_IS_STATUS_NOTE_ON(status))
    {
        uint8_t note = data[2];
        uint8_t velo = data[3];
        client->quequeMessage(MESSAGE_NOTE_ON, note, velo, 0.0);
    }

    return 0;
}

int CarlaBridgeOsc::handle_show()
{
    if (! client)
        return 1;

    client->quequeMessage(MESSAGE_SHOW_GUI, 1, 0, 0.0);

    return 0;
}

int CarlaBridgeOsc::handle_hide()
{
    if (! client)
        return 1;

    client->quequeMessage(MESSAGE_SHOW_GUI, 0, 0, 0.0);

    return 0;
}

int CarlaBridgeOsc::handle_quit()
{
    if (! client)
        return 1;

    client->quequeMessage(MESSAGE_QUIT, 0, 0, 0.0);

    return 0;
}

#ifdef BUILD_BRIDGE_PLUGIN
void osc_send_bridge_ains_peak(int index, double value)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+18];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_ains_peak");
        lo_send(global_osc_data.target, target_path, "if", index, value);
    }
}

void osc_send_bridge_aouts_peak(int index, double value)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+19];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_aouts_peak");
        lo_send(global_osc_data.target, target_path, "if", index, value);
    }
}

void osc_send_bridge_audio_count(int ins, int outs, int total)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+20];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_audio_count");
        lo_send(global_osc_data.target, target_path, "iii", ins, outs, total);
    }
}

void osc_send_bridge_midi_count(int ins, int outs, int total)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+19];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_midi_count");
        lo_send(global_osc_data.target, target_path, "iii", ins, outs, total);
    }
}

void osc_send_bridge_param_count(int ins, int outs, int total)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+20];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_param_count");
        lo_send(global_osc_data.target, target_path, "iii", ins, outs, total);
    }
}

void osc_send_bridge_program_count(int count)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+22];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_program_count");
        lo_send(global_osc_data.target, target_path, "i", count);
    }
}

void osc_send_bridge_midi_program_count(int count)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+27];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_midi_program_count");
        lo_send(global_osc_data.target, target_path, "i", count);
    }
}

void osc_send_bridge_plugin_info(int category, int hints, const char* name, const char* label, const char* maker, const char* copyright, long uniqueId)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+20];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_plugin_info");
        lo_send(global_osc_data.target, target_path, "iissssi", category, hints, name, label, maker, copyright, uniqueId);
        // FIXME - should be long type
    }
}

void osc_send_bridge_param_info(int index, const char* name, const char* unit)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+19];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_param_info");
        lo_send(global_osc_data.target, target_path, "iss", index, name, unit);
    }
}

void osc_send_bridge_param_data(int index, int type, int rindex, int hints, int midi_channel, int midi_cc)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+19];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_param_data");
        lo_send(global_osc_data.target, target_path, "iiiiii", index, type, rindex, hints, midi_channel, midi_cc);
    }
}

void osc_send_bridge_param_ranges(int index, double def, double min, double max, double step, double step_small, double step_large)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+21];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_param_ranges");
        lo_send(global_osc_data.target, target_path, "iffffff", index, def, min, max, step, step_small, step_large);
    }
}

void osc_send_bridge_program_info(int index, const char* name)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+21];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_program_info");
        lo_send(global_osc_data.target, target_path, "is", index, name);
    }
}

void osc_send_bridge_midi_program_info(int index, int bank, int program, const char* label)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+26];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_midi_program_info");
        lo_send(global_osc_data.target, target_path, "iiis", index, bank, program, label);
    }
}

void osc_send_bridge_custom_data(const char* stype, const char* key, const char* value)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+20];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_custom_data");
        lo_send(global_osc_data.target, target_path, "sss", stype, key, value);
    }
}

void osc_send_bridge_chunk_data(const char* string_data)
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+19];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_chunk_data");
        lo_send(global_osc_data.target, target_path, "s", string_data);
    }
}

void osc_send_bridge_update()
{
    if (global_osc_data.target)
    {
        char target_path[strlen(global_osc_data.path)+15];
        strcpy(target_path, global_osc_data.path);
        strcat(target_path, "/bridge_update");
        lo_send(global_osc_data.target, target_path, "");
    }
}
#endif

CARLA_BRIDGE_END_NAMESPACE
