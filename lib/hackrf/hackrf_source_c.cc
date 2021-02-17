/* -*- c++ -*- */
/*
 * Copyright 2013 Dimitri Stolnikov <horiz0n@gmx.net>
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdexcept>
#include <iostream>

#include <boost/assign.hpp>
#include <boost/format.hpp>
#include <boost/predef/other/endian.h>
#include <boost/algorithm/string.hpp>
#include <boost/thread/thread.hpp>

#include <gnuradio/io_signature.h>

#include "hackrf_source_c.h"

#include "arg_helpers.h"

using namespace boost::assign;

#define BUF_LEN  (16 * 32 * 512) /* must be multiple of 512 */
#define BUF_NUM   15

#define BYTES_PER_SAMPLE  2 /* HackRF device produces 8 bit unsigned IQ data */

#define HACKRF_FORMAT_ERROR(ret, msg) \
  boost::str( boost::format(msg " (%1%) %2%") \
    % ret % hackrf_error_name((enum hackrf_error)ret) )

#define HACKRF_THROW_ON_ERROR(ret, msg) \
  if ( ret != HACKRF_SUCCESS ) \
  { \
    throw std::runtime_error( HACKRF_FORMAT_ERROR(ret, msg) ); \
  }

#define HACKRF_FUNC_STR(func, arg) \
  boost::str(boost::format(func "(%1%)") % arg) + " has failed"

int hackrf_source_c::_usage = 0;
boost::mutex hackrf_source_c::_usage_mutex;

hackrf_source_c_sptr make_hackrf_source_c (const std::string & args)
{
  return gnuradio::get_initial_sptr(new hackrf_source_c (args));
}

/*
 * Specify constraints on number of input and output streams.
 * This info is used to construct the input and output signatures
 * (2nd & 3rd args to gr::block's constructor).  The input and
 * output signatures are used by the runtime system to
 * check that a valid number and type of inputs and outputs
 * are connected to this block.  In this case, we accept
 * only 0 input and 1 output.
 */
static const int MIN_IN = 0;	// mininum number of input streams
static const int MAX_IN = 0;	// maximum number of input streams
static const int MIN_OUT = 1;	// minimum number of output streams
static const int MAX_OUT = 1;	// maximum number of output streams

/*
 * The private constructor
 */
hackrf_source_c::hackrf_source_c (const std::string &args)
  : gr::sync_block ("hackrf_source_c",
        gr::io_signature::make(MIN_IN, MAX_IN, sizeof (gr_complex)),
        gr::io_signature::make(MIN_OUT, MAX_OUT, sizeof (gr_complex))),
    _dev(NULL),
    _buf(NULL),
    _sample_rate(0),
    _center_freq(0),
    _freq_corr(0),
    _auto_gain(false),
    _amp_gain(0),
    _lna_gain(0),
    _vga_gain(0),
    _bandwidth(0)
{
  int ret;
  std::string hackrf_serial;

  dict_t dict = params_to_dict(args);

  _buf_num = _buf_len = _buf_head = _buf_used = _buf_offset = 0;

  _biasT = false;

  if (dict.count("buffers"))
    _buf_num = boost::lexical_cast< unsigned int >( dict["buffers"] );

//  if (dict.count("buflen"))
//    _buf_len = boost::lexical_cast< unsigned int >( dict["buflen"] );

  if (0 == _buf_num)
    _buf_num = BUF_NUM;

  if (0 == _buf_len || _buf_len % 512 != 0) /* len must be multiple of 512 */
    _buf_len = BUF_LEN;

  _samp_avail = _buf_len / BYTES_PER_SAMPLE;

  // create a lookup table for gr_complex values
  for (unsigned int i = 0; i <= 0xffff; i++) {
#ifdef BOOST_ENDIAN_LITTLE_BYTE
    _lut.push_back( gr_complex( (float(int8_t(i & 0xff))) * (1.0f/128.0f),
                                (float(int8_t(i >> 8))) * (1.0f/128.0f) ) );
#else // BOOST_ENDIAN_LITTLE_BYTE
    _lut.push_back( gr_complex( (float(int8_t(i >> 8))) * (1.0f/128.0f),
                                (float(int8_t(i & 0xff))) * (1.0f/128.0f) ) );
#endif
  }

  {
    boost::mutex::scoped_lock lock( _usage_mutex );

    if ( _usage == 0 )
      hackrf_init(); /* call only once before the first open */

    _usage++;
  }

  _dev = NULL;
  
#ifdef LIBHACKRF_HAVE_DEVICE_LIST
  if (dict.count("hackrf") && dict["hackrf"].length() > 0) {
    hackrf_serial = dict["hackrf"];
    
    if (hackrf_serial.length() > 1) {
      ret = hackrf_open_by_serial( hackrf_serial.c_str(), &_dev );
    } else {
        int dev_index = 0;
        try {
          dev_index = boost::lexical_cast< int >( hackrf_serial );
        } catch ( std::exception &ex ) {
          throw std::runtime_error(
                "Failed to use '" + hackrf_serial + "' as HackRF device index number: " + ex.what());
        }

        hackrf_device_list_t *list = hackrf_device_list();
        if (dev_index < list->devicecount) {
          ret = hackrf_device_list_open(list, dev_index, &_dev);
        } else {
          hackrf_device_list_free(list);
          throw std::runtime_error(
                "Failed to use '" + hackrf_serial + "' as HackRF device index: not enough devices");
        }
        hackrf_device_list_free(list);
    }
  } else
#endif
    ret = hackrf_open( &_dev );
    
  HACKRF_THROW_ON_ERROR(ret, "Failed to open HackRF device")

  uint8_t board_id;
  ret = hackrf_board_id_read( _dev, &board_id );
  HACKRF_THROW_ON_ERROR(ret, "Failed to get HackRF board id")

  char version[40];
  memset(version, 0, sizeof(version));
  ret = hackrf_version_string_read( _dev, version, sizeof(version));
  HACKRF_THROW_ON_ERROR(ret, "Failed to read version string")

#if 0
  read_partid_serialno_t serial_number;
  ret = hackrf_board_partid_serialno_read( _dev, &serial_number );
  HACKRF_THROW_ON_ERROR(ret, "Failed to read serial number")
#endif
  std::cerr << "Using " << hackrf_board_id_name(hackrf_board_id(board_id)) << " "
            << "with firmware " << version << " "
            << std::endl;

  if ( BUF_NUM != _buf_num || BUF_LEN != _buf_len ) {
    std::cerr << "Using " << _buf_num << " buffers of size " << _buf_len << "."
              << std::endl;
  }

  set_center_freq( (get_freq_range().start() + get_freq_range().stop()) / 2.0 );
  set_sample_rate( get_sample_rates().start() );
  set_bandwidth( 0 );

  set_gain( 0 ); /* disable AMP gain stage by default to protect full sprectrum pre-amp from physical damage */

  set_if_gain( 16 ); /* preset to a reasonable default (non-GRC use case) */

  set_bb_gain( 20 ); /* preset to a reasonable default (non-GRC use case) */

  // Check device args to find out if bias/phantom power is desired.
  if ( dict.count("bias") ) {
    bool bias = boost::lexical_cast<bool>( dict["bias"] );
    ret = hackrf_set_antenna_enable(_dev, static_cast<uint8_t>(bias));
    if ( ret != HACKRF_SUCCESS )
    {
      std::cerr << "Failed to apply antenna bias voltage state: " << bias << HACKRF_FORMAT_ERROR(ret, "") << std::endl;
    }
    else
    {
      std::cerr << (bias ? "Enabled" : "Disabled") << " antenna bias voltage" << std::endl;
    }
  }

  _buf = (unsigned short **) malloc(_buf_num * sizeof(unsigned short *));

  if (_buf) {
    for(unsigned int i = 0; i < _buf_num; ++i)
      _buf[i] = (unsigned short *) malloc(_buf_len);
  }

//  _thread = gr::thread::thread(_hackrf_wait, this);

  ret = hackrf_start_rx( _dev, _hackrf_rx_callback, (void *)this );
  HACKRF_THROW_ON_ERROR(ret, "Failed to start RX streaming")
}

/*
 * Our virtual destructor.
 */
hackrf_source_c::~hackrf_source_c ()
{
  if (_dev) {
//    _thread.join();
    int ret = hackrf_stop_rx( _dev );
    if ( ret != HACKRF_SUCCESS )
    {
      std::cerr << HACKRF_FORMAT_ERROR(ret, "Failed to stop RX streaming") << std::endl;
    }
    ret = hackrf_close( _dev );
    if ( ret != HACKRF_SUCCESS )
    {
      std::cerr << HACKRF_FORMAT_ERROR(ret, "Failed to close HackRF") << std::endl;
    }
    _dev = NULL;

    {
      boost::mutex::scoped_lock lock( _usage_mutex );

       _usage--;

      if ( _usage == 0 )
        hackrf_exit(); /* call only once after last close */
    }
  }

  if (_buf) {
    for(unsigned int i = 0; i < _buf_num; ++i) {
      free(_buf[i]);
    }

    free(_buf);
    _buf = NULL;
  }
}

int hackrf_source_c::_hackrf_rx_callback(hackrf_transfer *transfer)
{
  hackrf_source_c *obj = (hackrf_source_c *)transfer->rx_ctx;
  return obj->hackrf_rx_callback(transfer->buffer, transfer->valid_length);
}

int hackrf_source_c::hackrf_rx_callback(unsigned char *buf, uint32_t len)
{
  {
    boost::mutex::scoped_lock lock( _buf_mutex );

    int buf_tail = (_buf_head + _buf_used) % _buf_num;
    memcpy(_buf[buf_tail], buf, len);

    if (_buf_used == _buf_num) {
      std::cerr << "O" << std::flush;
      _buf_head = (_buf_head + 1) % _buf_num;
    } else {
      _buf_used++;
    }
  }

  _buf_cond.notify_one();

  return 0; // TODO: return -1 on error/stop
}

void hackrf_source_c::_hackrf_wait(hackrf_source_c *obj)
{
  obj->hackrf_wait();
}

void hackrf_source_c::hackrf_wait()
{
}

bool hackrf_source_c::start()
{
  if ( ! _dev )
    return false;
#if 0
  int ret = hackrf_start_rx( _dev, _hackrf_rx_callback, (void *)this );
  if ( ret != HACKRF_SUCCESS ) {
    std::cerr << "Failed to start RX streaming (" << ret << ")" << std::endl;
    return false;
  }
#endif
  return true;
}

bool hackrf_source_c::stop()
{
  if ( ! _dev )
    return false;
#if 0
  int ret = hackrf_stop_rx( _dev );
  if ( ret != HACKRF_SUCCESS ) {
    std::cerr << "Failed to stop RX streaming (" << ret << ")" << std::endl;
    return false;
  }
#endif
  return true;
}

int hackrf_source_c::work( int noutput_items,
                        gr_vector_const_void_star &input_items,
                        gr_vector_void_star &output_items )
{
  gr_complex *out = (gr_complex *)output_items[0];

  bool running = false;

  if ( _dev )
    running = (hackrf_is_streaming( _dev ) == HACKRF_TRUE);

  {
    boost::mutex::scoped_lock lock( _buf_mutex );

    while (_buf_used < 3 && running) // collect at least 3 buffers
      _buf_cond.wait( lock );
  }

  if ( ! running )
    return WORK_DONE;

  unsigned short *buf = _buf[_buf_head] + _buf_offset;

  if (noutput_items <= _samp_avail) {
    for (int i = 0; i < noutput_items; ++i)
      *out++ = _lut[ *(buf + i) ];

    _buf_offset += noutput_items;
    _samp_avail -= noutput_items;
  } else {
    for (int i = 0; i < _samp_avail; ++i)
      *out++ = _lut[ *(buf + i) ];

    {
      boost::mutex::scoped_lock lock( _buf_mutex );

      _buf_head = (_buf_head + 1) % _buf_num;
      _buf_used--;
    }

    buf = _buf[_buf_head];

    int remaining = noutput_items - _samp_avail;

    for (int i = 0; i < remaining; ++i)
      *out++ = _lut[ *(buf + i) ];

    _buf_offset = remaining;
    _samp_avail = (_buf_len / BYTES_PER_SAMPLE) - remaining;
  }

  return noutput_items;
}

std::vector<std::string> hackrf_source_c::get_devices()
{
  std::vector<std::string> devices;
  std::string label;
  
  {
    boost::mutex::scoped_lock lock( _usage_mutex );

    if ( _usage == 0 )
      hackrf_init(); /* call only once before the first open */

    _usage++;
  }

#ifdef LIBHACKRF_HAVE_DEVICE_LIST
  hackrf_device_list_t *list = hackrf_device_list();
  
  for (int i = 0; i < list->devicecount; i++) {
    label = "HackRF ";
    label += hackrf_usb_board_id_name( list->usb_board_ids[i] );
    
    std::string args;
    if (list->serial_numbers[i]) {
      std::string serial = boost::lexical_cast< std::string >( list->serial_numbers[i] );
      if (serial.length() > 6)
        serial = serial.substr(serial.length() - 6, 6);
      args = "hackrf=" + serial;
      label += " " + serial;
    } else
      args = "hackrf"; /* will pick the first one, serial number is required for choosing a specific one */

    boost::algorithm::trim(label);

    args += ",label='" + label + "'";
    devices.push_back( args );
  }
  
  hackrf_device_list_free(list);
#else

  int ret;
  hackrf_device *dev = NULL;
  ret = hackrf_open(&dev);
  if ( HACKRF_SUCCESS == ret )
  {
    std::string args = "hackrf=0";

    label = "HackRF";

    uint8_t board_id;
    ret = hackrf_board_id_read( dev, &board_id );
    if ( HACKRF_SUCCESS == ret )
    {
      label += std::string(" ") + hackrf_board_id_name(hackrf_board_id(board_id));
    }

    args += ",label='" + label + "'";
    devices.push_back( args );

    ret = hackrf_close(dev);
  }

#endif

  {
    boost::mutex::scoped_lock lock( _usage_mutex );

     _usage--;

    if ( _usage == 0 )
      hackrf_exit(); /* call only once after last close */
  }

  return devices;
}

size_t hackrf_source_c::get_num_channels()
{
  return 1;
}

osmosdr::meta_range_t hackrf_source_c::get_sample_rates()
{
  osmosdr::meta_range_t range;

  /* we only add integer rates here because of better phase noise performance.
   * the user is allowed to request arbitrary (fractional) rates within these
   * boundaries. */

  range += osmosdr::range_t( 8e6 );
  range += osmosdr::range_t( 10e6 );
  range += osmosdr::range_t( 12.5e6 );
  range += osmosdr::range_t( 16e6 );
  range += osmosdr::range_t( 20e6 ); /* confirmed to work on fast machines */

  return range;
}

double hackrf_source_c::set_sample_rate( double rate )
{
  int ret;

  if (_dev) {
    ret = hackrf_set_sample_rate( _dev, rate );
    if ( HACKRF_SUCCESS == ret ) {
      _sample_rate = rate;
      //set_bandwidth( 0.0 ); /* bandwidth of 0 means automatic filter selection */
    } else {
      HACKRF_THROW_ON_ERROR( ret, HACKRF_FUNC_STR( "hackrf_set_sample_rate", rate ) )
    }
  }

  return get_sample_rate();
}

double hackrf_source_c::get_sample_rate()
{
  return _sample_rate;
}

osmosdr::freq_range_t hackrf_source_c::get_freq_range( size_t chan )
{
  osmosdr::freq_range_t range;

  range += osmosdr::range_t( _sample_rate / 2, 7250e6 - _sample_rate / 2 );

  return range;
}

double hackrf_source_c::set_center_freq( double freq, size_t chan )
{
  int ret;

  #define APPLY_PPM_CORR(val, ppm) ((val) * (1.0 + (ppm) * 0.000001))

  if (_dev) {
    double corr_freq = APPLY_PPM_CORR( freq, _freq_corr );
    ret = hackrf_set_freq( _dev, uint64_t(corr_freq) );
    if ( HACKRF_SUCCESS == ret ) {
      _center_freq = freq;
    } else {
      HACKRF_THROW_ON_ERROR( ret, HACKRF_FUNC_STR( "hackrf_set_freq", corr_freq ) )
    }
  }

  return get_center_freq( chan );
}

double hackrf_source_c::get_center_freq( size_t chan )
{
  return _center_freq;
}

double hackrf_source_c::set_freq_corr( double ppm, size_t chan )
{
  _freq_corr = ppm;

  set_center_freq( _center_freq );

  return get_freq_corr( chan );
}

double hackrf_source_c::get_freq_corr( size_t chan )
{
  return _freq_corr;
}

std::vector<std::string> hackrf_source_c::get_gain_names( size_t chan )
{
  std::vector< std::string > names;

  names += "RF";
  names += "IF";
  names += "BB";

  return names;
}

osmosdr::gain_range_t hackrf_source_c::get_gain_range( size_t chan )
{
  return get_gain_range( "RF", chan );
}

osmosdr::gain_range_t hackrf_source_c::get_gain_range( const std::string & name, size_t chan )
{
  if ( "RF" == name ) {
    return osmosdr::gain_range_t( 0, 14, 14 );
  }

  if ( "IF" == name ) {
    return osmosdr::gain_range_t( 0, 40, 8 );
  }

  if ( "BB" == name ) {
    return osmosdr::gain_range_t( 0, 62, 2 );
  }

  return osmosdr::gain_range_t();
}

bool hackrf_source_c::set_gain_mode( bool automatic, size_t chan )
{
  _auto_gain = automatic;

  return get_gain_mode(chan);
}

bool hackrf_source_c::get_gain_mode( size_t chan )
{
  return _auto_gain;
}

double hackrf_source_c::set_gain( double gain, size_t chan )
{
  int ret;
  osmosdr::gain_range_t rf_gains = get_gain_range( "RF", chan );

  if (_dev) {
    double clip_gain = rf_gains.clip( gain, true );
    uint8_t value = clip_gain == 14.0f ? 1 : 0;

    ret = hackrf_set_amp_enable( _dev, value );
    if ( HACKRF_SUCCESS == ret ) {
      _amp_gain = clip_gain;
    } else {
      HACKRF_THROW_ON_ERROR( ret, HACKRF_FUNC_STR( "hackrf_set_amp_enable", value ) )
    }
  }

  return _amp_gain;
}

double hackrf_source_c::set_gain( double gain, const std::string & name, size_t chan)
{
  if ( "RF" == name ) {
    return set_gain( gain, chan );
  }

  if ( "IF" == name ) {
    return set_if_gain( gain, chan );
  }

  if ( "BB" == name ) {
    return set_bb_gain( gain, chan );
  }

  return set_gain( gain, chan );
}

double hackrf_source_c::get_gain( size_t chan )
{
  return _amp_gain;
}

double hackrf_source_c::get_gain( const std::string & name, size_t chan )
{
  if ( "RF" == name ) {
    return get_gain( chan );
  }

  if ( "IF" == name ) {
    return _lna_gain;
  }

  if ( "BB" == name ) {
    return _vga_gain;
  }

  return get_gain( chan );
}

double hackrf_source_c::set_if_gain(double gain, size_t chan)
{
  int ret;
  osmosdr::gain_range_t rf_gains = get_gain_range( "IF", chan );

  if (_dev) {
    double clip_gain = rf_gains.clip( gain, true );

    ret = hackrf_set_lna_gain( _dev, uint32_t(clip_gain) );
    if ( HACKRF_SUCCESS == ret ) {
      _lna_gain = clip_gain;
    } else {
      HACKRF_THROW_ON_ERROR( ret, HACKRF_FUNC_STR( "hackrf_set_lna_gain", clip_gain ) )
    }
  }

  return _lna_gain;
}

double hackrf_source_c::set_bb_gain( double gain, size_t chan )
{
  int ret;
  osmosdr::gain_range_t if_gains = get_gain_range( "BB", chan );

  if (_dev) {
    double clip_gain = if_gains.clip( gain, true );

    ret = hackrf_set_vga_gain( _dev, uint32_t(clip_gain) );
    if ( HACKRF_SUCCESS == ret ) {
      _vga_gain = clip_gain;
    } else {
      HACKRF_THROW_ON_ERROR( ret, HACKRF_FUNC_STR( "hackrf_set_vga_gain", clip_gain ) )
    }
  }

  return _vga_gain;
}

std::vector< std::string > hackrf_source_c::get_antennas( size_t chan )
{
  std::vector< std::string > antennas;

  antennas += get_antenna( chan );

  return antennas;
}

std::string hackrf_source_c::set_antenna( const std::string & antenna, size_t chan )
{
  return get_antenna( chan );
}

std::string hackrf_source_c::get_antenna( size_t chan )
{
  return "TX/RX";
}

double hackrf_source_c::set_bandwidth( double bandwidth, size_t chan )
{
  int ret;
//  osmosdr::freq_range_t bandwidths = get_bandwidth_range( chan );

  if ( bandwidth == 0.0 ) /* bandwidth of 0 means automatic filter selection */
    bandwidth = _sample_rate * 0.75; /* select narrower filters to prevent aliasing */

  if ( _dev ) {
    /* compute best default value depending on sample rate (auto filter) */
    uint32_t bw = hackrf_compute_baseband_filter_bw( uint32_t(bandwidth) );
    ret = hackrf_set_baseband_filter_bandwidth( _dev, bw );
    if ( HACKRF_SUCCESS == ret ) {
      _bandwidth = bw;
    } else {
      HACKRF_THROW_ON_ERROR( ret, HACKRF_FUNC_STR( "hackrf_set_baseband_filter_bandwidth", bw ) )
    }
  }

  return _bandwidth;
}

double hackrf_source_c::get_bandwidth( size_t chan )
{
  return _bandwidth;
}

osmosdr::freq_range_t hackrf_source_c::get_bandwidth_range( size_t chan )
{
  osmosdr::freq_range_t bandwidths;

  // TODO: read out from libhackrf when an API is available

  bandwidths += osmosdr::range_t( 1750000 );
  bandwidths += osmosdr::range_t( 2500000 );
  bandwidths += osmosdr::range_t( 3500000 );
  bandwidths += osmosdr::range_t( 5000000 );
  bandwidths += osmosdr::range_t( 5500000 );
  bandwidths += osmosdr::range_t( 6000000 );
  bandwidths += osmosdr::range_t( 7000000 );
  bandwidths += osmosdr::range_t( 8000000 );
  bandwidths += osmosdr::range_t( 9000000 );
  bandwidths += osmosdr::range_t( 10000000 );
  bandwidths += osmosdr::range_t( 12000000 );
  bandwidths += osmosdr::range_t( 14000000 );
  bandwidths += osmosdr::range_t( 15000000 );
  bandwidths += osmosdr::range_t( 20000000 );
  bandwidths += osmosdr::range_t( 24000000 );
  bandwidths += osmosdr::range_t( 28000000 );

  return bandwidths;
}

void hackrf_source_c::set_biast( bool enabled ) {
  hackrf_set_antenna_enable(_dev, enabled ? 1 : 0);
  _biasT = enabled;
}

bool hackrf_source_c::get_biast() {
  return _biasT;
}
