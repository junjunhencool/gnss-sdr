/*!
 * \file galileo_e1_dll_pll_veml_tracking_cc.cc
 * \brief Implementation of a code DLL + carrier PLL VEML (Very Early
 *  Minus Late) tracking block for Galileo E1 signals
 * \author Luis Esteve, 2012. luis(at)epsilon-formacion.com
 *
 * Code DLL + carrier PLL according to the algorithms described in:
 * [1] K.Borre, D.M.Akos, N.Bertelsen, P.Rinder, and S.H.Jensen,
 * A Software-Defined GPS and Galileo Receiver. A Single-Frequency
 * Approach, Birkha user, 2007
 *
 * -------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2011  (see AUTHORS file for a list of contributors)
 *
 * GNSS-SDR is a software defined Global Navigation
 *          Satellite Systems receiver
 *
 * This file is part of GNSS-SDR.
 *
 * GNSS-SDR is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * at your option) any later version.
 *
 * GNSS-SDR is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNSS-SDR. If not, see <http://www.gnu.org/licenses/>.
 *
 * -------------------------------------------------------------------------
 */

#include "gnss_synchro.h"
#include "galileo_e1_dll_pll_veml_tracking_cc.h"
#include "galileo_e1_signal_processing.h"
#include "tracking_discriminators.h"
#include "CN_estimators.h"
#include "GPS_L1_CA.h"
#include "Galileo_E1.h"
#include "control_message_factory.h"
#include <boost/lexical_cast.hpp>
#include <iostream>
#include <sstream>
#include <cmath>
#include "math.h"
#include <gnuradio/gr_io_signature.h>
#include <glog/log_severity.h>
#include <glog/logging.h>

/*!
 * \todo Include in definition header file
 */
#define CN0_ESTIMATION_SAMPLES 10
#define MINIMUM_VALID_CN0 25
#define MAXIMUM_LOCK_FAIL_COUNTER 200


using google::LogMessage;

galileo_e1_dll_pll_veml_tracking_cc_sptr
galileo_e1_dll_pll_veml_make_tracking_cc(
        long if_freq,
        long fs_in,
        unsigned int vector_length,
        gr_msg_queue_sptr queue,
        bool dump,
        std::string dump_filename,
        float pll_bw_hz,
        float dll_bw_hz,
        float early_late_space_chips,
        float very_early_late_space_chips)
{
    return galileo_e1_dll_pll_veml_tracking_cc_sptr(new galileo_e1_dll_pll_veml_tracking_cc(if_freq,
            fs_in, vector_length, queue, dump, dump_filename, pll_bw_hz, dll_bw_hz, early_late_space_chips, very_early_late_space_chips));
}



void galileo_e1_dll_pll_veml_tracking_cc::forecast (int noutput_items,
        gr_vector_int &ninput_items_required)
{
    ninput_items_required[0] = (int)d_vector_length*2; //set the required available samples in each call
}



galileo_e1_dll_pll_veml_tracking_cc::galileo_e1_dll_pll_veml_tracking_cc(
        long if_freq,
        long fs_in,
        unsigned int vector_length,
        gr_msg_queue_sptr queue,
        bool dump,
        std::string dump_filename,
        float pll_bw_hz,
        float dll_bw_hz,
        float early_late_space_chips,
        float very_early_late_space_chips):
        gr_block ("galileo_e1_dll_pll_veml_tracking_cc", gr_make_io_signature (1, 1, sizeof(gr_complex)),
                gr_make_io_signature(1, 1, sizeof(Gnss_Synchro)))
{
    d_debug_counter = 0;
    this->set_relative_rate(1.0/vector_length);
    // initialize internal vars
    d_queue = queue;
    d_dump = dump;
    d_if_freq = if_freq;
    d_fs_in = fs_in;
    d_vector_length = vector_length;
    d_dump_filename = dump_filename;
    d_code_loop_filter=Tracking_2nd_DLL_filter(0.004);
    d_carrier_loop_filter=Tracking_2nd_PLL_filter(0.004);

    // Initialize tracking  ==========================================

    d_code_loop_filter.set_DLL_BW(dll_bw_hz);
    d_carrier_loop_filter.set_PLL_BW(pll_bw_hz);

    //--- DLL variables --------------------------------------------------------
    d_early_late_spc_chips = early_late_space_chips; // Define early-late offset (in chips)
    d_very_early_late_spc_chips = very_early_late_space_chips; // Define very-early-late offset (in chips)

    // Initialization of local code replica
    // Get space for a vector with the sinboc(1,1) replica sampled 2x/chip
//    int d_ca_code_size = (int)(2*Galileo_E1_B_CODE_LENGTH_CHIPS + 4);
    d_ca_code = new gr_complex[(int)(2*Galileo_E1_B_CODE_LENGTH_CHIPS + 4)];

//    std::cout << "d_ca_code_size = " << d_ca_code_size << std::endl;

    /* If an array is partitioned for more than one thread to operate on,
     * having the sub-array boundaries unaligned to cache lines could lead
     * to performance degradation. Here we allocate memory
     * (gr_comlex array of size 2*d_vector_length) aligned to cache of 16 bytes
     */
    // todo: do something if posix_memalign fails
    // Get space for the resampled early / prompt / late local replicas
    if (posix_memalign((void**)&d_very_early_code, 16, d_vector_length * sizeof(gr_complex) * 2) == 0){};
    if (posix_memalign((void**)&d_early_code, 16, d_vector_length * sizeof(gr_complex) * 2) == 0){};
    if (posix_memalign((void**)&d_prompt_code, 16, d_vector_length * sizeof(gr_complex) * 2) == 0){};
    if (posix_memalign((void**)&d_late_code, 16, d_vector_length * sizeof(gr_complex) * 2) == 0){};
    if (posix_memalign((void**)&d_very_late_code, 16, d_vector_length * sizeof(gr_complex) * 2) == 0){};
    // space for carrier wipeoff and signal baseband vectors
    if (posix_memalign((void**)&d_carr_sign, 16, d_vector_length * sizeof(gr_complex) * 2) == 0){};
    // correlator outputs (scalar)
    if (posix_memalign((void**)&d_Very_Early, 16, sizeof(gr_complex)) == 0){};
    if (posix_memalign((void**)&d_Early, 16, sizeof(gr_complex)) == 0){};
    if (posix_memalign((void**)&d_Prompt, 16, sizeof(gr_complex)) == 0){};
    if (posix_memalign((void**)&d_Late, 16, sizeof(gr_complex)) == 0){};
    if (posix_memalign((void**)&d_Very_Late, 16, sizeof(gr_complex)) == 0){};


    //--- Perform initializations ------------------------------
    // define initial code frequency basis of NCO
    d_code_freq_hz = Galileo_E1_CODE_CHIP_RATE_HZ;
    // define residual code phase (in chips)
    d_rem_code_phase_samples = 0.0;
    // define residual carrier phase
    d_rem_carr_phase_rad = 0.0;
    // define phase step
    d_code_phase_step_chips = d_code_freq_hz / (float)d_fs_in; //[chips]

    // sample synchronization
    d_sample_counter = 0;
    //d_sample_counter_seconds = 0;
    d_acq_sample_stamp = 0;

    d_enable_tracking = false;
    d_pull_in = false;
    d_last_seg = 0;

    d_current_prn_length_samples = (int)d_vector_length;

    // CN0 estimation and lock detector buffers
    d_cn0_estimation_counter = 0;
    d_Prompt_buffer = new gr_complex[CN0_ESTIMATION_SAMPLES];
    d_carrier_lock_test = 1;
    d_CN0_SNV_dB_Hz = 0;
    d_carrier_lock_fail_counter = 0;
    d_carrier_lock_threshold = 20;

    systemName["G"] = std::string("GPS");
    systemName["R"] = std::string("GLONASS");
    systemName["S"] = std::string("SBAS");
    systemName["E"] = std::string("Galileo");
    systemName["C"] = std::string("Compass");
}

void galileo_e1_dll_pll_veml_tracking_cc::start_tracking()
{

    d_acq_code_phase_samples = d_acquisition_gnss_synchro->Acq_delay_samples;
    d_acq_carrier_doppler_hz = d_acquisition_gnss_synchro->Acq_doppler_hz;
    d_acq_sample_stamp =  d_acquisition_gnss_synchro->Acq_samplestamp_samples;

//    std::cout << "d_acq_code_phase_samples = " << d_acq_code_phase_samples << std::endl;
//    std::cout << "d_acq_carrier_doppler_hz = " << d_acq_carrier_doppler_hz << std::endl;
//    std::cout << "d_acq_sample_stamp = " << d_acq_sample_stamp << std::endl;

    // DLL/PLL filter initialization
    d_carrier_loop_filter.initialize(d_acq_carrier_doppler_hz); //initialize the carrier filter
    d_code_loop_filter.initialize(d_acq_code_phase_samples); //initialize the code filter


    // generate local reference ALWAYS starting at chip 2 (2 samples per chip)

//    std::cout << "PRN = " << d_acquisition_gnss_synchro->PRN << std::endl;
//    std::cout << "Signal = " << d_acquisition_gnss_synchro->Signal << std::endl;
//    std::cout << "fs_gen = " << 2*Galileo_E1_CODE_CHIP_RATE_HZ << std::endl;
    galileo_e1_code_gen_complex_sampled(&d_ca_code[2],d_acquisition_gnss_synchro->Signal, false, d_acquisition_gnss_synchro->PRN, 2*Galileo_E1_CODE_CHIP_RATE_HZ, 0);

//    std::cout << "Local code generated." << std::endl;
//    for(int i=0;i<25; i++) std::cout << d_ca_code[i];
//           std::cout << std::endl;
//    for(int i=(int)(2*Galileo_E1_B_CODE_LENGTH_CHIPS-6);i<(int)(2*Galileo_E1_B_CODE_LENGTH_CHIPS+4); i++) std::cout << d_ca_code[i];
//                  std::cout << std::endl;
//
//    int index = (int)(2*Galileo_E1_B_CODE_LENGTH_CHIPS);
    d_ca_code[0] = d_ca_code[(int)(2*Galileo_E1_B_CODE_LENGTH_CHIPS)];
//    std::cout << "d_ca_code[0] = d_ca_code[" << index <<"]" << std::endl;
//    index = (int)(2*Galileo_E1_B_CODE_LENGTH_CHIPS+1);
    d_ca_code[1] = d_ca_code[(int)(2*Galileo_E1_B_CODE_LENGTH_CHIPS+1)];
//    std::cout << "d_ca_code[1] = d_ca_code[" << index <<"]" << std::endl;
//    index = (int)(2*Galileo_E1_B_CODE_LENGTH_CHIPS+2);
    d_ca_code[(int)(2*Galileo_E1_B_CODE_LENGTH_CHIPS+2)] = d_ca_code[2];
//    std::cout << "d_ca_code[" << index <<"] = d_ca_code[2]" << std::endl;
//    index = (int)(2*Galileo_E1_B_CODE_LENGTH_CHIPS+3);
    d_ca_code[(int)(2*Galileo_E1_B_CODE_LENGTH_CHIPS+3)] = d_ca_code[3];
//    std::cout << "d_ca_code[" << index <<"] = d_ca_code[3]" << std::endl;
//    for(int i=0;i<25; i++) std::cout << d_ca_code[i];
//       std::cout << std::endl;
    d_carrier_lock_fail_counter = 0;
    d_rem_code_phase_samples = 0.0;
    d_rem_carr_phase_rad = 0;
    d_next_rem_code_phase_samples = 0;
    d_acc_carrier_phase_rad = 0;

    d_code_phase_samples = d_acq_code_phase_samples;
    d_carrier_doppler_hz = d_acq_carrier_doppler_hz;

    d_next_prn_length_samples = d_vector_length;

    std::string sys_ = &d_acquisition_gnss_synchro->System;
    sys = sys_.substr(0,1);

    // DEBUG OUTPUT
    std::cout << "Tracking start on channel " << d_channel << " for satellite " << Gnss_Satellite(systemName[sys], d_acquisition_gnss_synchro->PRN) << std::endl;
    DLOG(INFO) << "Start tracking for satellite " << Gnss_Satellite(systemName[sys], d_acquisition_gnss_synchro->PRN)  << " received" << std::endl;

    // enable tracking
    d_pull_in = true;
    d_enable_tracking = true;

    std::cout << "PULL-IN Doppler [Hz]=" << d_carrier_doppler_hz
            << " PULL-IN Code Phase [samples]=" << d_acq_code_phase_samples << std::endl;
}





void galileo_e1_dll_pll_veml_tracking_cc::update_local_code()
{

    double tcode_half_chips;
    float rem_code_phase_half_chips;
    int associated_chip_index;
    int code_length_half_chips = (int)(2*Galileo_E1_B_CODE_LENGTH_CHIPS);
    double code_phase_step_chips;
    double code_phase_step_half_chips;
    int early_late_spc_samples;
    int very_early_late_spc_samples;
    int epl_loop_length_samples;

    // unified loop for E, P, L code vectors
    code_phase_step_chips = ((double)d_code_freq_hz) / ((double)d_fs_in);
    code_phase_step_half_chips = (2.0*(double)d_code_freq_hz) / ((double)d_fs_in);

    rem_code_phase_half_chips = d_rem_code_phase_samples * (2*d_code_freq_hz / d_fs_in);
    tcode_half_chips = -(double)rem_code_phase_half_chips;

    early_late_spc_samples=round(d_early_late_spc_chips/code_phase_step_chips);
    very_early_late_spc_samples=round(d_very_early_late_spc_chips/code_phase_step_chips);

    epl_loop_length_samples=d_current_prn_length_samples+very_early_late_spc_samples*2;

//    if(d_debug_counter<10){
//            std::cout << std::endl;
//            std::cout << "======= DEBUG " << d_debug_counter << " ========" << std::endl << std::endl;
//            std::cout << "rem_code_phase_half_chips = " << rem_code_phase_half_chips << std::endl;
//            std::cout << "code_phase_step_chips = " << code_phase_step_chips << std::endl;
//            std::cout << "code_phase_step_half_chips = " << code_phase_step_half_chips << std::endl;
//            std::cout << "early_late_spc_samples = " << early_late_spc_samples << std::endl;
//            std::cout << "very_early_late_spc_samples = " << very_early_late_spc_samples << std::endl;
//            std::cout << "d_current_prn_length_samples = " << d_current_prn_length_samples << std::endl;
//            std::cout << "epl_loop_length_samples = " << epl_loop_length_samples << std::endl << std::endl;
//    }

    for (int i=0; i<epl_loop_length_samples; i++)
        {
            associated_chip_index = 2 + round(fmod(tcode_half_chips - 2*d_very_early_late_spc_chips, code_length_half_chips));
//            if(d_debug_counter<4  && ((i<10)||(i==100)||(498<i && i<501)||(i==1000)||(i==5000)||(i==10000)||(i==20000)||(i==32000))) {
//                    std::cout << "tcode_half_chips = " << tcode_half_chips << ", i = " << i << ", associated_chip_index = " << associated_chip_index << std::endl;
//                    //std::cout << "tcode_half_chips - 2*d_very_early_late_spc_chips = " << tcode_half_chips - 2*d_very_early_late_spc_chips << ", i = " << i << ", associated_chip_index = " << associated_chip_index << std::endl;
//            }
            d_very_early_code[i] = d_ca_code[associated_chip_index];
            tcode_half_chips = tcode_half_chips + code_phase_step_half_chips;
        }
    memcpy(d_early_code,&d_very_early_code[very_early_late_spc_samples-early_late_spc_samples],d_current_prn_length_samples* sizeof(gr_complex));
    memcpy(d_prompt_code,&d_very_early_code[very_early_late_spc_samples],d_current_prn_length_samples* sizeof(gr_complex));
    memcpy(d_late_code,&d_very_early_code[2*very_early_late_spc_samples-early_late_spc_samples],d_current_prn_length_samples* sizeof(gr_complex));
    memcpy(d_very_late_code,&d_very_early_code[2*very_early_late_spc_samples],d_current_prn_length_samples* sizeof(gr_complex));

}




void galileo_e1_dll_pll_veml_tracking_cc::update_local_carrier()
{
    float phase_rad, phase_step_rad;

    phase_step_rad = (float)GPS_TWO_PI*d_carrier_doppler_hz / (float)d_fs_in;
    phase_rad = d_rem_carr_phase_rad;
    for(int i = 0; i < d_current_prn_length_samples; i++)
        {
            d_carr_sign[i] = gr_complex(cos(phase_rad), sin(phase_rad));
            phase_rad += phase_step_rad;
        }
    d_rem_carr_phase_rad = fmod(phase_rad, GPS_TWO_PI);
    d_acc_carrier_phase_rad = d_acc_carrier_phase_rad + d_rem_carr_phase_rad;
}




galileo_e1_dll_pll_veml_tracking_cc::~galileo_e1_dll_pll_veml_tracking_cc()
{
    d_dump_file.close();

    free(d_very_early_code);
    free(d_early_code);
    free(d_prompt_code);
    free(d_late_code);
    free(d_very_late_code);
    free(d_carr_sign);
    free(d_Very_Early);
    free(d_Early);
    free(d_Prompt);
    free(d_Late);
    free(d_Very_Late);

    delete[] d_ca_code;
    delete[] d_Prompt_buffer;
}


int galileo_e1_dll_pll_veml_tracking_cc::general_work (int noutput_items,gr_vector_int &ninput_items,
        gr_vector_const_void_star &input_items, gr_vector_void_star &output_items)
{

    // process vars
    float carr_error;
    float carr_nco;
    float code_error;
    float code_nco;

    if (d_enable_tracking == true)
        {
            /*
             * Receiver signal alignment
             */
            if (d_pull_in == true)
                {
                    int samples_offset;

                    float acq_trk_shif_correction_samples;
                    int acq_to_trk_delay_samples;
                    acq_to_trk_delay_samples = d_sample_counter - d_acq_sample_stamp;
                    acq_trk_shif_correction_samples = d_next_prn_length_samples - fmod((float)acq_to_trk_delay_samples, (float)d_next_prn_length_samples);
//                    std::cout<<"acq_trk_shif_correction="<<acq_trk_shif_correction_samples<< std::endl;
                    samples_offset = round(d_acq_code_phase_samples + acq_trk_shif_correction_samples);
                    // /todo: Check if the sample counter sent to the next block as a time reference should be incremented AFTER sended or BEFORE
                    //d_sample_counter_seconds = d_sample_counter_seconds + (((double)samples_offset) / (double)d_fs_in);
                    d_sample_counter = d_sample_counter + samples_offset; //count for the processed samples
                    d_pull_in = false;
//                    std::cout << "samples_offset=" << samples_offset << std::endl;
                    d_debug_counter++;
                    consume_each(samples_offset); //shift input to perform alignement with local replica
                    return 1;
                }

            // GNSS_SYNCHRO OBJECT to interchange data between tracking->telemetry_decoder
            Gnss_Synchro current_synchro_data;
            // Fill the acquisition data
            current_synchro_data = *d_acquisition_gnss_synchro;

            const gr_complex* in = (gr_complex*) input_items[0]; //PRN start block alignement
            Gnss_Synchro **out = (Gnss_Synchro **) &output_items[0];

//            if(d_debug_counter == 1) {
//                    for (int i=0; i<10; i++) std::cout << "in["<<i<<"] = "<< in[i] << std::endl;
//            }

            // Update the prn length based on code freq (variable) and
            // sampling frequency (fixed)
            // variable code PRN sample block size
            d_current_prn_length_samples = d_next_prn_length_samples;

            update_local_code();
            update_local_carrier();

            // perform Early, Prompt and Late correlation
            d_correlator.Carrier_wipeoff_and_VEPL_volk(d_current_prn_length_samples,
                    in,
                    d_carr_sign,
                    d_very_early_code,
                    d_early_code,
                    d_prompt_code,
                    d_late_code,
                    d_very_late_code,
                    d_Very_Early,
                    d_Early,
                    d_Prompt,
                    d_Late,
                    d_Very_Late,
                    is_unaligned());


            // Compute PLL error and update carrier NCO
            carr_error = pll_cloop_two_quadrant_atan(*d_Prompt) / (float)GPS_TWO_PI;
            // Implement carrier loop filter and generate NCO command
            carr_nco = d_carrier_loop_filter.get_carrier_nco(carr_error);
            // Modify carrier freq based on NCO command
            d_carrier_doppler_hz = d_acq_carrier_doppler_hz + carr_nco;
            //std::cout << "d_carrier_doppler_hz = " << d_carrier_doppler_hz << std::endl;
            // Compute DLL error and update code NCO
            code_error = dll_nc_vemlp_normalized(*d_Very_Early, *d_Early, *d_Late, *d_Very_Late);
            // Implement code loop filter and generate NCO command
            code_nco = d_code_loop_filter.get_code_nco(code_error);
            // Modify code freq based on NCO command
            d_code_freq_hz = Galileo_E1_CODE_CHIP_RATE_HZ - code_nco;
            // Update the phase step based on code freq (variable) and
            // sampling frequency (fixed)
            d_code_phase_step_chips = d_code_freq_hz / (float)d_fs_in; //[chips]
            // variable code PRN sample block size
            float T_chip_seconds;
            float T_prn_seconds;
            float T_prn_samples;
            float K_blk_samples;
            T_chip_seconds = 1 / d_code_freq_hz;
            T_prn_seconds = T_chip_seconds * Galileo_E1_B_CODE_LENGTH_CHIPS;
            T_prn_samples = T_prn_seconds * d_fs_in;
            d_rem_code_phase_samples = d_next_rem_code_phase_samples;
            K_blk_samples = T_prn_samples + d_rem_code_phase_samples;
            d_next_prn_length_samples = round(K_blk_samples); //round to a discrete samples
            d_next_rem_code_phase_samples = K_blk_samples - d_next_prn_length_samples; //rounding error

//            if(d_debug_counter<10){
//                    std::cout << std::endl;
//                    std::cout << "----- LOOP RESULTS -----" << std::endl;
//                    std::cout << "carr_error = " << carr_error << std::endl;
//                    std::cout << "carr_nco = " << carr_nco << std::endl;
//                    std::cout << "d_carrier_doppler_hz = " << d_carrier_doppler_hz << std::endl;
//                    std::cout << "code_error = " << code_error << std::endl;
//                    std::cout << "code_nco = " << code_nco << std::endl;
//                    std::cout << "d_code_freq_hz = " << d_code_freq_hz << std::endl;
//                    std::cout << "d_code_phase_step_chips = " << d_code_phase_step_chips << std::endl;
//                    std::cout << "d_rem_code_phase_samples = " << d_rem_code_phase_samples << std::endl;
//            }

            /*!
             * \todo Improve the lock detection algorithm!
             */
            // ####### CN0 ESTIMATION AND LOCK DETECTORS ######
            if (d_cn0_estimation_counter < CN0_ESTIMATION_SAMPLES)
                {
                    // fill buffer with prompt correlator output values
                    d_Prompt_buffer[d_cn0_estimation_counter] = *d_Prompt;
                    d_cn0_estimation_counter++;
                }
            else
                {
                    d_cn0_estimation_counter = 0;
                    d_CN0_SNV_dB_Hz = galileo_e1_CN0_SNV(d_Prompt_buffer, CN0_ESTIMATION_SAMPLES, d_fs_in);
                    d_carrier_lock_test = carrier_lock_detector(d_Prompt_buffer, CN0_ESTIMATION_SAMPLES);
                    // ###### TRACKING UNLOCK NOTIFICATION #####
                    if (std::abs(d_carrier_lock_test) > d_carrier_lock_threshold or d_CN0_SNV_dB_Hz < MINIMUM_VALID_CN0)
                        {
                            d_carrier_lock_fail_counter++;
                        }
                    else
                        {
                            if (d_carrier_lock_fail_counter > 0) d_carrier_lock_fail_counter--;
                        }
                    if (d_carrier_lock_fail_counter > MAXIMUM_LOCK_FAIL_COUNTER)
                        {
                            std::cout << "Channel " << d_channel << " loss of lock!" << std::endl ;
                            //tracking_message = 3; //loss of lock
                            //d_channel_internal_queue->push(tracking_message);
                            ControlMessageFactory* cmf = new ControlMessageFactory();
                            if (d_queue != gr_msg_queue_sptr()) {
                                d_queue->handle(cmf->GetQueueMessage(d_channel, 2));
                            }
                            delete cmf;
                            d_carrier_lock_fail_counter = 0;
                            d_enable_tracking = false; // TODO: check if disabling tracking is consistent with the channel state machine

                        }
                    //std::cout<<"d_carrier_lock_fail_counter"<<d_carrier_lock_fail_counter<<"\r\n";
                }

            // ########### Output the tracking data to navigation and PVT ##########

            current_synchro_data.Prompt_I = (double)(*d_Prompt).imag();
            current_synchro_data.Prompt_Q = (double)(*d_Prompt).real();
            // Tracking_timestamp_secs is aligned with the PRN start sample
            current_synchro_data.Tracking_timestamp_secs=((double)d_sample_counter+(double)d_next_prn_length_samples+(double)d_next_rem_code_phase_samples)/(double)d_fs_in;
            // This tracking block aligns the Tracking_timestamp_secs with the start sample of the PRN, thus, Code_phase_secs=0
            current_synchro_data.Code_phase_secs=0;
            current_synchro_data.Carrier_phase_rads = (double)d_acc_carrier_phase_rad;
            current_synchro_data.CN0_dB_hz = (double)d_CN0_SNV_dB_Hz;
            *out[0] = current_synchro_data;

            // ########## DEBUG OUTPUT
            /*!
             *  \todo The stop timer has to be moved to the signal source!
             */
            // debug: Second counter in channel 0
            if (d_channel == 0)
                {
                    if (floor(d_sample_counter / d_fs_in) != d_last_seg)
                        {
                            d_last_seg = floor(d_sample_counter / d_fs_in);
                            std::cout << "Current input signal time = " << d_last_seg << " [s]" << std::endl;
                            std::cout << "Tracking CH " << d_channel <<  ": Satellite " << Gnss_Satellite(systemName[sys], d_acquisition_gnss_synchro->PRN)
                                                    << ", CN0 = " << d_CN0_SNV_dB_Hz << " [dB-Hz]" << std::endl;
                            //std::cout<<"TRK CH "<<d_channel<<" Carrier_lock_test="<<d_carrier_lock_test<< std::endl;
                            //if (d_last_seg==5) d_carrier_lock_fail_counter=500; //DEBUG: force unlock!
                        }
                }
            else
                {
                    if (floor(d_sample_counter / d_fs_in) != d_last_seg)
                        {
                            d_last_seg = floor(d_sample_counter / d_fs_in);
                            std::cout << "Tracking CH " << d_channel <<  ": Satellite " << Gnss_Satellite(systemName[sys], d_acquisition_gnss_synchro->PRN)
                                                    << ", CN0 = " << d_CN0_SNV_dB_Hz << " [dB-Hz]" << std::endl;
                        }
                }
        }
    else
        {
            *d_Early = gr_complex(0,0);
            *d_Prompt = gr_complex(0,0);
            *d_Late = gr_complex(0,0);
            Gnss_Synchro **out = (Gnss_Synchro **) &output_items[0]; //block output streams pointer
            //std::cout<<output_items.size()<<std::endl;
            // GNSS_SYNCHRO OBJECT to interchange data between tracking->telemetry_decoder
            Gnss_Synchro current_synchro_data;
            *out[0] = current_synchro_data;
        }

    if(d_dump)
        {
            // MULTIPLEXED FILE RECORDING - Record results to file
            float prompt_I;
            float prompt_Q;
            float tmp_VE, tmp_E, tmp_P, tmp_L, tmp_VL;
            float tmp_float;
            double tmp_double;
            prompt_I = (*d_Prompt).imag();
            prompt_Q = (*d_Prompt).real();
            tmp_VE = std::abs<float>(*d_Very_Early);
            tmp_E = std::abs<float>(*d_Early);
            tmp_P = std::abs<float>(*d_Prompt);
            tmp_L = std::abs<float>(*d_Late);
            tmp_VL = std::abs<float>(*d_Very_Late);

            try
            {
                    // EPR
                    d_dump_file.write((char*)&tmp_VE, sizeof(float));
                    d_dump_file.write((char*)&tmp_E, sizeof(float));
                    d_dump_file.write((char*)&tmp_P, sizeof(float));
                    d_dump_file.write((char*)&tmp_L, sizeof(float));
                    d_dump_file.write((char*)&tmp_VL, sizeof(float));
                    // PROMPT I and Q (to analyze navigation symbols)
                    d_dump_file.write((char*)&prompt_I, sizeof(float));
                    d_dump_file.write((char*)&prompt_Q, sizeof(float));
                    // PRN start sample stamp
                    d_dump_file.write((char*)&d_sample_counter, sizeof(unsigned long int));
                    // accumulated carrier phase
                    d_dump_file.write((char*)&d_acc_carrier_phase_rad, sizeof(float));

                    // carrier and code frequency
                    d_dump_file.write((char*)&d_carrier_doppler_hz, sizeof(float));
                    d_dump_file.write((char*)&d_code_freq_hz, sizeof(float));

                    //PLL commands
                    d_dump_file.write((char*)&carr_error, sizeof(float));
                    d_dump_file.write((char*)&carr_nco, sizeof(float));

                    //DLL commands
                    d_dump_file.write((char*)&code_error, sizeof(float));
                    d_dump_file.write((char*)&code_nco, sizeof(float));

                    // CN0 and carrier lock test
                    d_dump_file.write((char*)&d_CN0_SNV_dB_Hz, sizeof(float));
                    d_dump_file.write((char*)&d_carrier_lock_test, sizeof(float));

                    // AUX vars (for debug purposes)
                    tmp_float = d_rem_code_phase_samples;
                    d_dump_file.write((char*)&tmp_float, sizeof(float));
                    tmp_double=(double)(d_sample_counter+d_current_prn_length_samples);
                    d_dump_file.write((char*)&tmp_double, sizeof(double));
//                    if(d_debug_counter < 10){
//                            std::cout << std::endl;
//                            std::cout << "d_debug_counter = " << d_debug_counter << std::endl;
//                            std::cout << "VE = " << tmp_VE << ", E = " << tmp_E << ", P = "<< tmp_P << ", L = " << tmp_L << ", VL = " << tmp_VL << std::endl << std::endl;
//                    }
            }
            catch (std::ifstream::failure e)
            {
                    std::cout << "Exception writing trk dump file " << e.what() << std::endl;
            }
        }
//    if(d_current_prn_length_samples!=d_vector_length)
//    std::cout << "d_current_prn_length_samples = " << d_current_prn_length_samples << std::endl;
    consume_each(d_current_prn_length_samples); // this is necesary in gr_block derivates
    d_sample_counter += d_current_prn_length_samples; //count for the processed samples
    d_debug_counter++;
    return 1; //output tracking result ALWAYS even in the case of d_enable_tracking==false
}



void galileo_e1_dll_pll_veml_tracking_cc::set_channel(unsigned int channel)
{
    d_channel = channel;
    LOG_AT_LEVEL(INFO) << "Tracking Channel set to " << d_channel;
    // ############# ENABLE DATA FILE LOG #################
    if (d_dump==true)
        {
            if (d_dump_file.is_open() == false)
                {
                    try
                    {
                            d_dump_filename.append(boost::lexical_cast<std::string>(d_channel));
                            d_dump_filename.append(".dat");
                            d_dump_file.exceptions (std::ifstream::failbit | std::ifstream::badbit);
                            d_dump_file.open(d_dump_filename.c_str(), std::ios::out | std::ios::binary);
                            std::cout << "Tracking dump enabled on channel " << d_channel << " Log file: " << d_dump_filename.c_str() << std::endl;
                    }
                    catch (std::ifstream::failure e)
                    {
                            std::cout << "channel " << d_channel << " Exception opening trk dump file " << e.what() << std::endl;
                    }
                }
        }
}



void galileo_e1_dll_pll_veml_tracking_cc::set_channel_queue(concurrent_queue<int> *channel_internal_queue)
{
    d_channel_internal_queue = channel_internal_queue;
}

void galileo_e1_dll_pll_veml_tracking_cc::set_gnss_synchro(Gnss_Synchro* p_gnss_synchro)
{
    d_acquisition_gnss_synchro = p_gnss_synchro;

    //  Gnss_Satellite(satellite.get_system(), satellite.get_PRN());
    //DLOG(INFO) << "Tracking code phase set to " << d_acq_code_phase_samples;
    //DLOG(INFO) << "Tracking carrier doppler set to " << d_acq_carrier_doppler_hz;
    //DLOG(INFO) << "Tracking Satellite set to " << d_satellite;

}
