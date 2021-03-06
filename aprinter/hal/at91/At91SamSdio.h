/*
 * Copyright (c) 2015 Ambroz Bizjak
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef APRINTER_AT91SAM_SDIO_H
#define APRINTER_AT91SAM_SDIO_H

#include <stdint.h>

#include <pmc.h>
#include <dmac.h>

#include <aprinter/meta/MinMax.h>
#include <aprinter/meta/ServiceUtils.h>
#include <aprinter/base/Object.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/base/Assert.h>
#include <aprinter/misc/ClockUtils.h>
#include <aprinter/hal/generic/SdioInterface.h>
#include <aprinter/hal/at91/At91SamPins.h>

#include <aprinter/BeginNamespace.h>

template <typename Arg>
class At91SamSdio {
    using Context        = typename Arg::Context;
    using ParentObject   = typename Arg::ParentObject;
    using CommandHandler = typename Arg::CommandHandler;
    using BusyTimeout    = typename Arg::BusyTimeout;
    using Params         = typename Arg::Params;
    
public:
    struct Object;
    
private:
    static_assert(Params::Slot == 0 || Params::Slot == 1, "");
    static_assert(Params::MaxIoDescriptors > 0, "");
    
    using TheClockUtils = ClockUtils<Context>;
    using TheDebugObject = DebugObject<Context, Object>;
    using FastEvent = typename Context::EventLoop::template FastEventSpec<At91SamSdio>;
    
    enum {INIT_STATE_OFF, INIT_STATE_POWERON, INIT_STATE_ON};
    enum {CMD_STATE_READY, CMD_STATE_BUSY, CMD_STATE_WAIT_BUSY};
    enum {DATA_STATE_READY, DATA_STATE_WAIT_XFRDONE, DATA_STATE_WAIT_DMA};
    
    static constexpr double InitSpeed = 400000.0;
    static constexpr double FullSpeed = 25000000.0;
    
    static constexpr uint32_t ClkdivForSpeed (double speed)
    {
        return (uint32_t)MaxValue(0.0, (double)F_MCK / (2.0 * speed) - 0.001);
    }
    
    static typename TheClockUtils::TimeType const DmaTimeoutTicks = 0.5 * TheClockUtils::time_freq;
    static typename TheClockUtils::TimeType const BusyTimeoutTicks = BusyTimeout::value() * TheClockUtils::time_freq;
    
#if defined(__SAM3X8E__)
    static int const DmaChannel = 0;
    static int const DmaHwId = 0;
    
    static void chip_specific_config (Context c)
    {
        Context::Pins::template setPeripheral<At91SamPin<At91SamPioA, 19>>(c, At91SamPeriphA());
        Context::Pins::template setPeripheral<At91SamPin<At91SamPioA, 20>>(c, At91SamPeriphA());
        Context::Pins::template setPeripheral<At91SamPin<At91SamPioA, 21>>(c, At91SamPeriphA());
        Context::Pins::template setPeripheral<At91SamPin<At91SamPioA, 22>>(c, At91SamPeriphA());
        Context::Pins::template setPeripheral<At91SamPin<At91SamPioA, 23>>(c, At91SamPeriphA());
        Context::Pins::template setPeripheral<At91SamPin<At91SamPioA, 24>>(c, At91SamPeriphA());
    }
#elif defined(__SAM3U4E__)
    static int const DmaChannel = 0;
    static int const DmaHwId = 0;
    
    static void chip_specific_config (Context c)
    {
        Context::Pins::template setPeripheral<At91SamPin<At91SamPioA, 3>>(c, At91SamPeriphA());
        Context::Pins::template setPeripheral<At91SamPin<At91SamPioA, 4>>(c, At91SamPeriphA());
        Context::Pins::template setPeripheral<At91SamPin<At91SamPioA, 5>>(c, At91SamPeriphA());
        Context::Pins::template setPeripheral<At91SamPin<At91SamPioA, 6>>(c, At91SamPeriphA());
        Context::Pins::template setPeripheral<At91SamPin<At91SamPioA, 7>>(c, At91SamPeriphA());
        Context::Pins::template setPeripheral<At91SamPin<At91SamPioA, 8>>(c, At91SamPeriphA());
    }
#else
#error "Unsupported device"
#endif
    
public:
    static bool const IsWideMode = Params::IsWideMode;
    static size_t const BlockSize = 512;
    static size_t const MaxIoBlocks = UINT16_MAX;
    static int const MaxIoDescriptors = Params::MaxIoDescriptors;
    
    static void init (Context c)
    {
        auto *o = Object::self(c);
        
        chip_specific_config(c);
        
        Context::EventLoop::template initFastEvent<FastEvent>(c, At91SamSdio::event_handler);
        
        o->init_state = INIT_STATE_OFF;
        
        TheDebugObject::init(c);
    }
    
    static void deinit (Context c)
    {
        TheDebugObject::deinit(c);
        
        reset_internal(c);
    }
    
    static void reset (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        
        reset_internal(c);
        o->init_state = INIT_STATE_OFF;
    }
    
    static void startPowerOn (Context c, SdioIface::InterfaceParams if_params)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(o->init_state == INIT_STATE_OFF)
        
        pmc_enable_periph_clk(ID_HSMCI);
        pmc_enable_periph_clk(ID_DMAC);
        
        HSMCI->HSMCI_CR = HSMCI_CR_SWRST;
        
        HSMCI->HSMCI_DTOR = HSMCI_DTOR_DTOMUL_1048576 | HSMCI_DTOR_DTOCYC(2);
        HSMCI->HSMCI_CSTOR = HSMCI_CSTOR_CSTOMUL_1048576 | HSMCI_CSTOR_CSTOCYC(2);
        HSMCI->HSMCI_CFG = HSMCI_CFG_FIFOMODE | HSMCI_CFG_FERRCTRL;
        HSMCI->HSMCI_MR = HSMCI_MR_PWSDIV_Msk;
        HSMCI->HSMCI_CR = HSMCI_CR_MCIEN | HSMCI_CR_PWSEN;
        
        configure_interface(if_params);
        
        // The current driver model doesn't quite fit here, since it assumes we'll be sending
        // the initialization clocks to the card all the time until completePowerOn(), but
        // this hardware has a special command to send just the required number of clocks.
        // So start this command now, and by the time completePowerOn() is called, it will
        // surely be done.
        HSMCI->HSMCI_MR &= ~(HSMCI_MR_WRPROOF | HSMCI_MR_RDPROOF | HSMCI_MR_FBYTE);
        HSMCI->HSMCI_ARGR = 0;
        HSMCI->HSMCI_CMDR = HSMCI_CMDR_RSPTYP_NORESP | HSMCI_CMDR_SPCMD_INIT | HSMCI_CMDR_OPDCMD_OPENDRAIN;
        
        o->init_state = INIT_STATE_POWERON;
    }
    
    static void completePowerOn (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(o->init_state == INIT_STATE_POWERON)
        
        o->init_state = INIT_STATE_ON;
        o->pending = false;
        o->cmd_state = CMD_STATE_READY;
        o->data_state = DATA_STATE_READY;
    }
    
    static void reconfigureInterface (Context c, SdioIface::InterfaceParams if_params)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(o->init_state == INIT_STATE_ON)
        AMBRO_ASSERT(!o->pending)
        AMBRO_ASSERT(o->cmd_state == CMD_STATE_READY)
        AMBRO_ASSERT(o->data_state == DATA_STATE_READY)
        
        configure_interface(if_params);
    }
    
    static void startCommand (Context c, SdioIface::CommandParams cmd_params)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(o->init_state == INIT_STATE_ON)
        AMBRO_ASSERT(!o->pending)
        AMBRO_ASSERT(o->cmd_state == CMD_STATE_READY)
        AMBRO_ASSERT(o->data_state == DATA_STATE_READY)
        
        uint32_t cmdr = HSMCI_CMDR_CMDNB(cmd_params.cmd_index) | HSMCI_CMDR_SPCMD_STD;
        
        switch (cmd_params.response_type) {
            case SdioIface::RESPONSE_NONE:
                break;
            case SdioIface::RESPONSE_SHORT:
                cmdr |= HSMCI_CMDR_MAXLAT | HSMCI_CMDR_RSPTYP_48_BIT;
                break;
            case SdioIface::RESPONSE_SHORT_BUSY:
                cmdr |= HSMCI_CMDR_MAXLAT | HSMCI_CMDR_RSPTYP_R1B;
                break;
            case SdioIface::RESPONSE_LONG:
                cmdr |= HSMCI_CMDR_MAXLAT | HSMCI_CMDR_RSPTYP_136_BIT;
                break;
            default:
                AMBRO_ASSERT(0);
        }
        
        if (cmd_params.direction != SdioIface::DATA_DIR_NONE) {
            AMBRO_ASSERT(cmd_params.direction == SdioIface::DATA_DIR_READ || cmd_params.direction == SdioIface::DATA_DIR_WRITE)
            AMBRO_ASSERT(cmd_params.num_blocks >= 1)
            AMBRO_ASSERT(cmd_params.num_blocks <= MaxIoBlocks)
            AMBRO_ASSERT(cmd_params.data_vector.num_descriptors <= MaxIoDescriptors)
            AMBRO_ASSERT(CheckTransferVector(cmd_params.data_vector, cmd_params.num_blocks * (size_t)(BlockSize/4)))
            
            bool is_read = cmd_params.direction == SdioIface::DATA_DIR_READ;
            
            dmac_enable(DMAC);
            AMBRO_ASSERT(!dmac_channel_is_enable(DMAC, DmaChannel))
            
            uint32_t dma_cfg = DMAC_CFG_SOD_ENABLE | DMAC_CFG_AHB_PROT(1) | DMAC_CFG_FIFOCFG_ALAP_CFG;
            if (is_read) {
                dma_cfg |= DMAC_CFG_SRC_H2SEL | DMAC_CFG_SRC_PER(DmaHwId);
                cmdr |= HSMCI_CMDR_TRDIR_READ;
            } else {
                dma_cfg |= DMAC_CFG_DST_H2SEL | DMAC_CFG_DST_PER(DmaHwId);
                cmdr |= HSMCI_CMDR_TRDIR_WRITE;
            }
            
            cmdr |= HSMCI_CMDR_TRCMD_START_DATA;
            cmdr |= (cmd_params.num_blocks == 1) ? HSMCI_CMDR_TRTYP_SINGLE : HSMCI_CMDR_TRTYP_MULTIPLE;
            
            dmac_channel_set_configuration(DMAC, DmaChannel, dma_cfg);
            
            for (int i = 0; i < cmd_params.data_vector.num_descriptors; i++) {
                auto *desc = &cmd_params.data_vector.descriptors[i];
                
                auto *hwdesc = &o->descriptors[i];
                hwdesc->ul_source_addr = is_read ? (uint32_t)&HSMCI->HSMCI_RDR : (uint32_t)desc->buffer_ptr;
                hwdesc->ul_destination_addr = is_read ? (uint32_t)desc->buffer_ptr : (uint32_t)&HSMCI->HSMCI_TDR;
                hwdesc->ul_ctrlA = DMAC_CTRLA_BTSIZE(desc->num_words) | DMAC_CTRLA_SRC_WIDTH_WORD | DMAC_CTRLA_DST_WIDTH_WORD;
                hwdesc->ul_ctrlB = DMAC_CTRLB_SRC_DSCR_FETCH_FROM_MEM | DMAC_CTRLB_DST_DSCR_FETCH_FROM_MEM | DMAC_CTRLB_IEN;
                if (is_read) {
                    hwdesc->ul_ctrlB |= DMAC_CTRLB_FC_PER2MEM_DMA_FC | DMAC_CTRLB_SRC_INCR_FIXED | DMAC_CTRLB_DST_INCR_INCREMENTING;
                } else {
                    hwdesc->ul_ctrlB |= DMAC_CTRLB_FC_MEM2PER_DMA_FC | DMAC_CTRLB_SRC_INCR_INCREMENTING | DMAC_CTRLB_DST_INCR_FIXED;
                }
                hwdesc->ul_descriptor_addr = (uint32_t)&o->descriptors[i + 1];
            }
            
            auto *last_hwdesc = &o->descriptors[cmd_params.data_vector.num_descriptors - 1];
            last_hwdesc->ul_descriptor_addr = 0;
            
            memory_barrier_dma();
            
            dmac_channel_multi_buf_transfer_init(DMAC, DmaChannel, o->descriptors);
            dmac_channel_enable(DMAC, DmaChannel);
            
            o->data_state = DATA_STATE_WAIT_XFRDONE;
            o->data_dir = cmd_params.direction;
            
            HSMCI->HSMCI_DMA = HSMCI_DMA_DMAEN;
            HSMCI->HSMCI_MR |= HSMCI_MR_WRPROOF | HSMCI_MR_RDPROOF;
            HSMCI->HSMCI_MR &= ~HSMCI_MR_FBYTE;
            HSMCI->HSMCI_BLKR = ((uint32_t)BlockSize << HSMCI_BLKR_BLKLEN_Pos) | ((uint32_t)cmd_params.num_blocks << HSMCI_BLKR_BCNT_Pos);
        } else {
            HSMCI->HSMCI_DMA = 0;
            HSMCI->HSMCI_MR &= ~(HSMCI_MR_WRPROOF | HSMCI_MR_RDPROOF | HSMCI_MR_FBYTE);
            HSMCI->HSMCI_BLKR = 0;
        }
        
        HSMCI->HSMCI_ARGR = cmd_params.argument;
        HSMCI->HSMCI_CMDR = cmdr;
        
        o->cmd_state = CMD_STATE_BUSY;
        o->cmd_response_type = cmd_params.response_type;
        o->cmd_flags = cmd_params.flags;
        o->pending = true;
        o->results = SdioIface::CommandResults{SdioIface::CMD_ERROR_NONE};
        o->data_error = SdioIface::DATA_ERROR_NONE;
        
        Context::EventLoop::template triggerFastEvent<FastEvent>(c);
    }
    
    using EventLoopFastEvents = MakeTypeList<FastEvent>;
    
private:
    static void configure_interface (SdioIface::InterfaceParams if_params)
    {
        uint32_t hsmci_clkdiv = if_params.clock_full_speed ? ClkdivForSpeed(FullSpeed) : ClkdivForSpeed(InitSpeed);
        uint32_t hsmci_slot = Params::Slot == 0 ? HSMCI_SDCR_SDCSEL_SLOTA : HSMCI_SDCR_SDCSEL_SLOTB;
        uint32_t hsmci_bus_width = Params::IsWideMode ? HSMCI_SDCR_SDCBUS_4 : HSMCI_SDCR_SDCBUS_1;
        
        HSMCI->HSMCI_MR &= ~HSMCI_MR_CLKDIV_Msk;
        HSMCI->HSMCI_MR |= HSMCI_MR_CLKDIV(hsmci_clkdiv);
        HSMCI->HSMCI_SDCR = hsmci_slot | hsmci_bus_width;
    }
    
    static void reset_internal (Context c)
    {
        auto *o = Object::self(c);
        
        if (o->init_state != INIT_STATE_OFF) {
            if (o->data_state != DATA_STATE_READY) {
                dmac_channel_disable(DMAC, DmaChannel);
                while (dmac_channel_is_enable(DMAC, DmaChannel));
            }
            HSMCI->HSMCI_CR = HSMCI_CR_MCIDIS;
            pmc_disable_periph_clk(ID_HSMCI);
        }
        
        Context::EventLoop::template resetFastEvent<FastEvent>(c);
    }
    
    static void event_handler (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(o->init_state == INIT_STATE_ON)
        AMBRO_ASSERT(o->pending)
        AMBRO_ASSERT(o->cmd_state != CMD_STATE_READY || o->data_state != DATA_STATE_READY)
        
        uint32_t status = HSMCI->HSMCI_SR;
        work_cmd(c, status);
        work_data(c, status);
        
        if (o->cmd_state != CMD_STATE_READY || o->data_state != DATA_STATE_READY) {
            Context::EventLoop::template triggerFastEvent<FastEvent>(c);
            return;
        }
        
        o->pending = false;
        
        return CommandHandler::call(c, o->results, o->data_error);
    }
    
    static void work_cmd (Context c, uint32_t status)
    {
        auto *o = Object::self(c);
        
        while (o->cmd_state != CMD_STATE_READY) {
            switch (o->cmd_state) {
                case CMD_STATE_BUSY: {
                    if (!(status & HSMCI_SR_CMDRDY)) {
                        return;
                    }
                    
                    if (o->cmd_response_type != SdioIface::RESPONSE_NONE) {
                        if (!(o->cmd_flags & SdioIface::CMD_FLAG_NO_CRC_CHECK)) {
                            if ((status & HSMCI_SR_RCRCE)) {
                                o->results.error_code = SdioIface::CMD_ERROR_RESPONSE_CHECKSUM;
                                goto cmd_done;
                            }
                        }
                        
                        if ((status & HSMCI_SR_RTOE)) {
                            o->results.error_code = SdioIface::CMD_ERROR_RESPONSE_TIMEOUT;
                            goto cmd_done;
                        }
                        
                        if (!(o->cmd_flags & SdioIface::CMD_FLAG_NO_CMDNUM_CHECK)) {
                            if ((status & HSMCI_SR_RINDE)) {
                                o->results.error_code = SdioIface::CMD_ERROR_BAD_RESPONSE_CMD;
                                goto cmd_done;
                            }
                        }
                        
                        if ((status & HSMCI_SR_CSTOE) || (status & HSMCI_SR_RENDE) || (status & HSMCI_SR_RDIRE)) {
                            o->results.error_code = SdioIface::CMD_ERROR_OTHER;
                            goto cmd_done;
                        }
                        
                        o->results.response[0] = HSMCI->HSMCI_RSPR[0];
                        
                        if (o->cmd_response_type == SdioIface::RESPONSE_LONG) {
                            o->results.response[1] = HSMCI->HSMCI_RSPR[0];
                            o->results.response[2] = HSMCI->HSMCI_RSPR[0];
                            o->results.response[3] = HSMCI->HSMCI_RSPR[0];
                        }
                        
                        if (o->cmd_response_type == SdioIface::RESPONSE_SHORT_BUSY) {
                            o->cmd_state = CMD_STATE_WAIT_BUSY;
                            o->busy_poll_timer.setAfter(c, BusyTimeoutTicks);
                            break;
                        }
                    }
                    
                cmd_done:
                    o->cmd_state = CMD_STATE_READY;
                } break;
                
                case CMD_STATE_WAIT_BUSY: {
                    if (!(status & HSMCI_SR_NOTBUSY)) {
                        if (!(o->busy_poll_timer.isExpired(c))) {
                            return;
                        }
                        o->results.error_code = SdioIface::CMD_ERROR_BUSY_TIMEOUT;
                    }
                    o->cmd_state = CMD_STATE_READY;
                } break;
                
                default: AMBRO_ASSERT(false);
            }
        }
    }
    
    static void work_data (Context c, uint32_t status)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->data_state == DATA_STATE_READY || o->data_state == DATA_STATE_WAIT_XFRDONE || o->data_state == DATA_STATE_WAIT_DMA)
        
        if (o->data_state == DATA_STATE_READY) {
            return;
        }
        
        if (o->data_state == DATA_STATE_WAIT_XFRDONE) {
            if ((status & HSMCI_SR_DCRCE)) {
                o->data_error = SdioIface::DATA_ERROR_CHECKSUM;
            }
            else if ((status & HSMCI_SR_DTOE)) {
                o->data_error = SdioIface::DATA_ERROR_TIMEOUT;
            }
            else if ((status & HSMCI_SR_OVRE)) {
                o->data_error = SdioIface::DATA_ERROR_RX_OVERRUN;
            }
            else if ((status & HSMCI_SR_UNRE)) {
                o->data_error = SdioIface::DATA_ERROR_TX_OVERRUN;
            }
            else if (!(status & HSMCI_SR_XFRDONE)) {
                return;
            }
            
            o->data_state = DATA_STATE_WAIT_DMA;
            o->poll_timer.setAfter(c, DmaTimeoutTicks);
        }
        
        if (o->data_state == DATA_STATE_WAIT_DMA) {
            if (o->data_error == SdioIface::DATA_ERROR_NONE) {
                if (!dmac_channel_is_transfer_done(DMAC, DmaChannel)) {
                    if (!o->poll_timer.isExpired(c)) {
                        return;
                    }
                    o->data_error = SdioIface::DATA_ERROR_DMA;
                }
            }
            
            dmac_channel_disable(DMAC, DmaChannel);
            while (dmac_channel_is_enable(DMAC, DmaChannel));
            
            if (o->data_dir == SdioIface::DATA_DIR_READ) {
                memory_barrier_dma();
            }
            
            o->data_state = DATA_STATE_READY;
        }
    }
    
public:
    struct Object : public ObjBase<At91SamSdio, ParentObject, MakeTypeList<
        TheDebugObject
    >> {
        uint8_t init_state;
        bool pending;
        uint8_t cmd_state;
        uint8_t data_state;
        uint8_t cmd_response_type;
        uint8_t cmd_flags;
        uint8_t data_dir;
        SdioIface::CommandResults results;
        SdioIface::DataErrorCode data_error;
        typename TheClockUtils::PollTimer poll_timer;
        typename TheClockUtils::PollTimer busy_poll_timer;
        dma_transfer_descriptor_t descriptors[MaxIoDescriptors];
    };
};

template <
    uint8_t TSlot,
    bool TIsWideMode,
    int TMaxIoDescriptors
>
struct At91SamSdioService {
    static uint8_t const Slot = TSlot;
    static bool const IsWideMode = TIsWideMode;
    static int const MaxIoDescriptors = TMaxIoDescriptors;
    
    APRINTER_ALIAS_STRUCT_EXT(Sdio, (
        APRINTER_AS_TYPE(Context),
        APRINTER_AS_TYPE(ParentObject),
        APRINTER_AS_TYPE(CommandHandler),
        APRINTER_AS_TYPE(BusyTimeout)
    ), (
        using Params = At91SamSdioService;
        APRINTER_DEF_INSTANCE(Sdio, At91SamSdio)
    ))
};

#include <aprinter/EndNamespace.h>

#endif
