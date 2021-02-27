// SPDX-License-Identifier: GPL-2.0+
/*******************************************************************************
 * QtMips - MIPS 32-bit Architecture Subset Simulator
 *
 * Implemented to support following courses:
 *
 *   B35APO - Computer Architectures
 *   https://cw.fel.cvut.cz/wiki/courses/b35apo
 *
 *   B4M35PAP - Advanced Computer Architectures
 *   https://cw.fel.cvut.cz/wiki/courses/b4m35pap/start
 *
 * Copyright (c) 2017-2019 Karel Koci<cynerd@email.cz>
 * Copyright (c) 2019      Pavel Pisa <pisa@cmp.felk.cvut.cz>
 *
 * Faculty of Electrical Engineering (http://www.fel.cvut.cz)
 * Czech Technical University        (http://www.cvut.cz/)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 ******************************************************************************/

#include "qtmipsmachine.h"

#include "programloader.h"

#include <QTime>
#include <utility>

using namespace machine;

QtMipsMachine::QtMipsMachine(
    MachineConfig config,
    bool load_symtab,
    bool load_executable)
    : machine_config(std::move(config))
    , stat(ST_READY) {
    regs = new Registers();

    if (load_executable) {
        ProgramLoader program(machine_config.elf());
        mem_program_only = new Memory(machine_config.get_simulated_endian());
        program.to_memory(mem_program_only);

        if (load_symtab) {
            symtab = program.get_symbol_table();
        }
        program_end = program.end();
        if (program.get_executable_entry() != 0x0_addr) {
            regs->pc_abs_jmp(program.get_executable_entry());
        }
        mem = new Memory(*mem_program_only);
    } else {
        mem = new Memory(machine_config.get_simulated_endian());
    }

    data_bus = new MemoryDataBus(machine_config.get_simulated_endian());
    data_bus->insert_device_to_range(
        mem, 0x00000000_addr, 0xefffffff_addr, false);

    setup_serial_port();
    setup_perip_spi_led();
    setup_lcd_display();

    cch_program = new Cache(
        data_bus, &machine_config.cache_program(),
        machine_config.memory_access_time_read(),
        machine_config.memory_access_time_write(),
        machine_config.memory_access_time_burst());
    cch_data = new Cache(
        data_bus, &machine_config.cache_data(),
        machine_config.memory_access_time_read(),
        machine_config.memory_access_time_write(),
        machine_config.memory_access_time_burst());

    unsigned int min_cache_row_size = 16;
    if (machine_config.cache_data().enabled()) {
        min_cache_row_size = machine_config.cache_data().block_size() * 4;
    }

    if (machine_config.cache_program().enabled()
        && machine_config.cache_program().block_size() < min_cache_row_size) {
        min_cache_row_size = machine_config.cache_program().block_size() * 4;
    }

    cop0st = new Cop0State();

    if (machine_config.pipelined()) {
        cr = new CorePipelined(
            regs, cch_program, cch_data, machine_config.hazard_unit(),
            min_cache_row_size, cop0st);
    } else {
        cr = new CoreSingle(
            regs, cch_program, cch_data, machine_config.delay_slot(),
            min_cache_row_size, cop0st);
    }
    connect(
        this, &QtMipsMachine::set_interrupt_signal, cop0st,
        &Cop0State::set_interrupt_signal);

    run_t = new QTimer(this);
    set_speed(0); // In default run as fast as possible
    connect(run_t, &QTimer::timeout, this, &QtMipsMachine::step_timer);

    for (int i = 0; i < EXCAUSE_COUNT; i++) {
        if (i != EXCAUSE_INT && i != EXCAUSE_BREAK && i != EXCAUSE_HWBREAK) {
            set_stop_on_exception(
                (enum ExceptionCause)i, machine_config.osemu_exception_stop());
            set_step_over_exception(
                (enum ExceptionCause)i, machine_config.osemu_exception_stop());
        }
    }

    set_stop_on_exception(EXCAUSE_INT, machine_config.osemu_interrupt_stop());
    set_step_over_exception(EXCAUSE_INT, false);
}
void QtMipsMachine::setup_lcd_display() {
    perip_lcd_display = new LcdDisplay(machine_config.get_simulated_endian());
    memory_bus_insert_range(
        perip_lcd_display, 0xffe00000_addr, 0xffe4afff_addr, true);
}
void QtMipsMachine::setup_perip_spi_led() {
    perip_spi_led = new PeripSpiLed(machine_config.get_simulated_endian());
    memory_bus_insert_range(
        perip_spi_led, 0xffffc100_addr, 0xffffc1ff_addr, true);
}
void QtMipsMachine::setup_serial_port() {
    ser_port = new SerialPort(machine_config.get_simulated_endian());
    memory_bus_insert_range(ser_port, 0xffffc000_addr, 0xffffc03f_addr, true);
    memory_bus_insert_range(ser_port, 0xffff0000_addr, 0xffff003f_addr, false);
    connect(
        ser_port, &SerialPort::signal_interrupt, this,
        &QtMipsMachine::set_interrupt_signal);
}

QtMipsMachine::~QtMipsMachine() {
    delete run_t;
    run_t = nullptr;
    delete cr;
    cr = nullptr;
    delete cop0st;
    cop0st = nullptr;
    delete regs;
    regs = nullptr;
    delete mem;
    mem = nullptr;
    delete cch_program;
    cch_program = nullptr;
    delete cch_data;
    cch_data = nullptr;
    delete data_bus;
    data_bus = nullptr;
    delete mem_program_only;
    mem_program_only = nullptr;
    delete symtab;
    symtab = nullptr;
}

const MachineConfig &QtMipsMachine::config() {
    return machine_config;
}

void QtMipsMachine::set_speed(unsigned int ips, unsigned int time_chunk) {
    this->time_chunk = time_chunk;
    run_t->setInterval(ips);
}

const Registers *QtMipsMachine::registers() {
    return regs;
}

const Cop0State *QtMipsMachine::cop0state() {
    return cop0st;
}

const Memory *QtMipsMachine::memory() {
    return mem;
}

Memory *QtMipsMachine::memory_rw() {
    return mem;
}

const Cache *QtMipsMachine::cache_program() {
    return cch_program;
}

const Cache *QtMipsMachine::cache_data() {
    return cch_data;
}

Cache *QtMipsMachine::cache_data_rw() {
    return cch_data;
}

void QtMipsMachine::cache_sync() {
    if (cch_program != nullptr) {
        cch_program->sync();
    }
    if (cch_data != nullptr) {
        cch_data->sync();
    }
}

const MemoryDataBus *QtMipsMachine::memory_data_bus() {
    return data_bus;
}

MemoryDataBus *QtMipsMachine::memory_data_bus_rw() {
    return data_bus;
}

SerialPort *QtMipsMachine::serial_port() {
    return ser_port;
}

PeripSpiLed *QtMipsMachine::peripheral_spi_led() {
    return perip_spi_led;
}

LcdDisplay *QtMipsMachine::peripheral_lcd_display() {
    return perip_lcd_display;
}

SymbolTable *QtMipsMachine::symbol_table_rw(bool create) {
    if (create && (symtab == nullptr)) {
        symtab = new SymbolTable;
    }
    return symtab;
}

const SymbolTable *QtMipsMachine::symbol_table(bool create) {
    return symbol_table_rw(create);
}

void QtMipsMachine::set_symbol(
    const QString &name,
    uint32_t value,
    uint32_t size,
    unsigned char info,
    unsigned char other) {
    if (symtab == nullptr) {
        symtab = new SymbolTable;
    }
    symtab->set_symbol(name, value, size, info, other);
}

const Core *QtMipsMachine::core() {
    return cr;
}

const CoreSingle *QtMipsMachine::core_singe() {
    return machine_config.pipelined() ? nullptr : (const CoreSingle *)cr;
}

const CorePipelined *QtMipsMachine::core_pipelined() {
    return machine_config.pipelined() ? (const CorePipelined *)cr : nullptr;
}

bool QtMipsMachine::executable_loaded() const {
    return (mem_program_only != nullptr);
}

enum QtMipsMachine::Status QtMipsMachine::status() {
    return stat;
}

bool QtMipsMachine::exited() {
    return stat == ST_EXIT || stat == ST_TRAPPED;
}

// We don't allow to call control methods when machine exited or if it's busy
// We rather silently fail.
#define CTL_GUARD                                                              \
    do {                                                                       \
        if (exited() || stat == ST_BUSY)                                       \
            return;                                                            \
    } while (false)

void QtMipsMachine::play() {
    CTL_GUARD;
    set_status(ST_RUNNING);
    run_t->start();
    step_internal(true);
}

void QtMipsMachine::pause() {
    if (stat != ST_BUSY) {
        CTL_GUARD;
    }
    set_status(ST_READY);
    run_t->stop();
}

void QtMipsMachine::step_internal(bool skip_break) {
    CTL_GUARD;
    enum Status stat_prev = stat;
    set_status(ST_BUSY);
    emit tick();
    try {
        QTime start_time = QTime::currentTime();
        do {
            cr->step(skip_break);
        } while (time_chunk != 0 && stat == ST_BUSY && !skip_break
                 && start_time.msecsTo(QTime::currentTime()) < (int)time_chunk);
    } catch (QtMipsException &e) {
        run_t->stop();
        set_status(ST_TRAPPED);
        emit program_trap(e);
        return;
    }
    if (regs->read_pc() >= program_end) {
        run_t->stop();
        set_status(ST_EXIT);
        emit program_exit();
    } else {
        if (stat == ST_BUSY) {
            set_status(stat_prev);
        }
    }
    emit post_tick();
}

void QtMipsMachine::step() {
    step_internal(true);
}

void QtMipsMachine::step_timer() {
    step_internal();
}

void QtMipsMachine::restart() {
    pause();
    regs->reset();
    if (mem_program_only != nullptr) {
        mem->reset(*mem_program_only);
    }
    cch_program->reset();
    cch_data->reset();
    cr->reset();
    set_status(ST_READY);
}

void QtMipsMachine::set_status(enum Status st) {
    bool change = st != stat;
    stat = st;
    if (change) {
        emit status_change(st);
    }
}

void QtMipsMachine::register_exception_handler(
    ExceptionCause excause,
    ExceptionHandler *exhandler) {
    if (cr != nullptr) {
        cr->register_exception_handler(excause, exhandler);
    }
}

bool QtMipsMachine::memory_bus_insert_range(
    BackendMemory *mem_acces,
    Address start_addr,
    Address last_addr,
    bool move_ownership) {
    if (data_bus == nullptr) {
        return false;
    }
    return data_bus->insert_device_to_range(
        mem_acces, start_addr, last_addr, move_ownership);
}

void QtMipsMachine::insert_hwbreak(Address address) {
    if (cr != nullptr) {
        cr->insert_hwbreak(address);
    }
}

void QtMipsMachine::remove_hwbreak(Address address) {
    if (cr != nullptr) {
        cr->remove_hwbreak(address);
    }
}

bool QtMipsMachine::is_hwbreak(Address address) {
    if (cr != nullptr) {
        return cr->is_hwbreak(address);
    }
    return false;
}

void QtMipsMachine::set_stop_on_exception(
    enum ExceptionCause excause,
    bool value) {
    if (cr != nullptr) {
        cr->set_stop_on_exception(excause, value);
    }
}

bool QtMipsMachine::get_stop_on_exception(enum ExceptionCause excause) const {
    if (cr != nullptr) {
        return cr->get_stop_on_exception(excause);
    }
    return false;
}

void QtMipsMachine::set_step_over_exception(
    enum ExceptionCause excause,
    bool value) {
    if (cr != nullptr) {
        cr->set_step_over_exception(excause, value);
    }
}

bool QtMipsMachine::get_step_over_exception(enum ExceptionCause excause) const {
    if (cr != nullptr) {
        return cr->get_step_over_exception(excause);
    }
    return false;
}

enum ExceptionCause QtMipsMachine::get_exception_cause() const {
    uint32_t val;
    if (cop0st == nullptr) {
        return EXCAUSE_NONE;
    }
    val = (cop0st->read_cop0reg(Cop0State::Cause) >> 2) & 0x3f;
    if (val == 0) {
        return EXCAUSE_INT;
    } else {
        return (ExceptionCause)val;
    }
}
