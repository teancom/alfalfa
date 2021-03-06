#include <unistd.h>
#include <string>
#include <assert.h>
#include <list>

#include "network.h"
#include "select.h"

#include "deliveryforecast.pb.h"

using namespace std;
using namespace Network;

class BulkPacket
{
private:
  string _forecast, _data;

public:
  BulkPacket() : _forecast(), _data() {}

  /* No forecast update */
  BulkPacket( bool, const string & s_data )
    : _forecast(),
      _data( s_data )
  {}

  void add_forecast( const Sprout::DeliveryForecast & s_forecast )
  {
    _forecast = s_forecast.SerializeAsString();
    assert( _forecast.size() <= 65535 );
  }

  BulkPacket( const string & incoming )
    : _forecast( incoming.substr( sizeof( uint16_t ), *(uint16_t *) incoming.data() ) ),
      _data( incoming.substr( sizeof( uint16_t ) + _forecast.size() ) )
  {}

  string tostring( void ) const
  {
    uint16_t forecast_size = _forecast.size();
    string ret( (char *) & forecast_size, sizeof( forecast_size ) );
    if ( forecast_size ) {
      ret.append( _forecast );
    }
    ret.append( _data );
  
    return ret;
  }

  const string & raw_forecast( void ) const { return _forecast; }
  bool has_forecast( void ) const { return _forecast.size() > 0; }

  Sprout::DeliveryForecast forecast( void ) const
  {
    assert( has_forecast() );

    Sprout::DeliveryForecast ret;
    assert( ret.ParseFromString( _forecast ) );

    return ret;
  }
};

int main( int argc, char *argv[] )
{
  char *key;
  char *ip;
  int port;

  Network::Connection *net;

  bool server = true;

  if ( argc > 1 ) {
    /* client */

    server = false;

    key = argv[ 1 ];
    ip = argv[ 2 ];
    port = atoi( argv[ 3 ] );

    net = new Network::Connection( key, ip, port );
  } else {
    net = new Network::Connection( NULL, NULL );
  }

  fprintf( stderr, "Port bound is %d, key is %s\n", net->port(), net->get_key().c_str() );

  Select &sel = Select::get_instance();
  sel.add_fd( net->fd() );

  const int fallback_interval = 5000;
  const int TARGET_DELAY_TICKS = 5;

  /* wait to get attached */
  if ( server ) {
    while ( 1 ) {
      int active_fds = sel.select( -1 );
      if ( active_fds < 0 ) {
	perror( "select" );
	exit( 1 );
      }

      if ( sel.read( net->fd() ) ) {
	net->recv();
      }

      if ( net->get_has_remote_addr() ) {
	break;
      }
    }
  }

  uint64_t time_of_last_forecast = -1;
  uint64_t time_of_next_transmission = timestamp() + fallback_interval;

  fprintf( stderr, "Looping...\n" );  

  Sprout::DeliveryForecast operative_forecast = net->forecast(); /* initial but valid forecast */
  uint64_t forecast_timestamp = 0;

  /* loop */
  while ( 1 ) {
    /* possibly send packets */
    uint64_t delayed_queue_estimate = net->get_next_seq() - operative_forecast.received_or_lost_count();
    int current_forecast_tick = (timestamp() - forecast_timestamp) / net->get_tick_length();

    if ( current_forecast_tick < 0 ) {
      current_forecast_tick = 0;
    } else if ( current_forecast_tick >= operative_forecast.counts_size() ) {
      current_forecast_tick = operative_forecast.counts_size() - 1;
    }

    int current_queue_estimate = delayed_queue_estimate - operative_forecast.counts( current_forecast_tick );
    if ( current_queue_estimate < 0 ) {
      current_queue_estimate = 0;
    }

    //    fprintf( stderr, "Current queue estimate: %d\n", current_queue_estimate );

    int cumulative_delivery_tick = current_forecast_tick + TARGET_DELAY_TICKS;
    if ( cumulative_delivery_tick >= operative_forecast.counts_size() ) {
      cumulative_delivery_tick = operative_forecast.counts_size() - 1;
    }

    int cumulative_delivery_forecast = operative_forecast.counts( cumulative_delivery_tick ) - operative_forecast.counts( current_forecast_tick );

    int packets_to_send = cumulative_delivery_forecast - current_queue_estimate;

    fprintf( stderr, "DeQueEst = %d, CurQueEst = %d, SRTT=%f, Current tick=%d (%d packets), target tick=%d (%d packets), sending %d packets\n",
	     (int)delayed_queue_estimate, (int)current_queue_estimate, net->get_SRTT(), current_forecast_tick, operative_forecast.counts( current_forecast_tick ),
	     cumulative_delivery_tick, cumulative_delivery_forecast, packets_to_send );


    if ( packets_to_send < 0 ) {
      packets_to_send = 0;
    }

    /*
    fprintf( stderr, "current tick = %d, cum delivery tick = %d, packets to send = %d\n",
	     current_forecast_tick, cumulative_delivery_tick, packets_to_send );
    */

    /* actually send, maybe */
    Sprout::DeliveryForecast forecast = net->forecast();
    if ( ( packets_to_send > 0 ) || ( time_of_next_transmission <= timestamp() ) ) {
      //      fprintf( stderr, "Sending %d packets\n", packets_to_send );

      do {
	string data( 1400, ' ' );
	assert( data.size() == 1400 );

	BulkPacket bp( false, data );

	if ( forecast.time() != time_of_last_forecast ) {
	  bp.add_forecast( forecast );
	  time_of_last_forecast = forecast.time();

	  /*
	  fprintf( stderr, "Sent counts:" );
	  for ( int i = 0; i < forecast.counts_size(); i++ ) {
	    fprintf( stderr, " %d", forecast.counts( i ) );
	  }
	  fprintf( stderr, "\n" );
	  */
	}

	uint16_t time_to_next = 0;
	if ( packets_to_send <= 1 ) {
	  time_to_next = 500;
	}

	net->send( bp.tostring(), time_to_next );
	if ( packets_to_send > 0 ) { packets_to_send--; }
      } while ( packets_to_send > 0 );

      time_of_next_transmission = std::max( timestamp() + fallback_interval,
					    time_of_next_transmission );
    }

    /* wait */
    int wait_time = time_of_next_transmission - timestamp();
    if ( wait_time < 0 ) {
      wait_time = 0;
    } else if ( wait_time > 10 ) {
      wait_time = 10;
    }

    int active_fds = sel.select( wait_time );
    if ( active_fds < 0 ) {
      perror( "select" );
      exit( 1 );
    }

    /* receive */
    if ( sel.read( net->fd() ) ) {
      BulkPacket packet( net->recv() );
      
      if ( packet.has_forecast() ) {
	operative_forecast = packet.forecast();

	forecast_timestamp = timestamp(); // - (net->get_SRTT() / 2.0);

	fprintf( stderr, "Received counts:" );
	for ( int i = 0; i < operative_forecast.counts_size(); i++ ) {
	  fprintf( stderr, " %d", operative_forecast.counts( i ) );
	}
	fprintf( stderr, "\n" );
      }
    }
  }
}
