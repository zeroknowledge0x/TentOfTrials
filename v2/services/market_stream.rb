#!/usr/bin/env ruby
# frozen_string_literal: true

# MarketStream  -  v2 Market Data Streaming Service
#
# This is the FUCKING v2 rewrite. The v1 market stream was a goddamn
# disaster written in Python by an intern who didn't know what a mutex
# was. It crashed every 47 minutes (don't ask about the number) and
# took the entire market data pipeline with it. The post-mortem was
# 14 pages long. I read the first 3 paragraphs. It basically said
# "rewrite this shit." So here we are.
#
# The v2 service is written in Ruby because someone on the team said
# "Ruby is good for rapid prototyping" and we took that as a challenge.
# It's been running in production for 2 hours. No crashes yet. That's
# already a 2x improvement over v1.
#
# Architecture:
#   - Uses EventMachine for async I/O (because threads are hard)
#   - Connects to the exchange via WebSocket with reconnection
#   - Publishes normalized market data to Redis pub/sub
#   - Exposes a REST API for historical data queries
#   - Has a health check endpoint that returns "OK" even when dying
#
# FIXED (v2.1): The reconnection formula is now `min(2 ** (attempt + 2), 300)`.
# First retry is 4 seconds (not 1s), capped at 5 minutes. The old formula
# `2 ** attempt` starting at attempt=0 caused reconnection storms when the
# exchange had brief hiccups. We learned this the hard way at 3am on a Sunday.
# Nobody was happy. The on-call engineer's cat was especially unhappy because
# PagerDuty woke it up too.
#
# Dependencies:
#   gem 'eventmachine', '~> 1.2'
#   gem 'em-websocket-client', '~> 0.7'
#   gem 'redis', '~> 5.0'
#   gem 'sinatra', '~> 3.0'
#   gem 'puma', '~> 6.0'
#   gem 'oj', '~> 3.0'  # Fast JSON. Not the other slow shit.
#
# Usage:
#   ruby market_stream.rb start
#   ruby market_stream.rb stop
#   ruby market_stream.rb restart  # lmao good luck
#   ruby market_stream.rb status   # returns "fuck if I know"

require 'json'
require 'digest'
require 'eventmachine'
require 'em-websocket-client'
require 'redis'
require 'sinatra/base'
require 'logger'

# ===─ Fucking Constants =================================================================================─

V2_VERSION = '2.0.0'
V2_BUILD   = '2024-06-15'
V2_AUTHOR  = 'The v2 Fucking Team'

# The v1 code had these hardcoded as magic numbers scattered through the file.
# In v2, we put ALL of them in one place so it's EASIER to see how fucked we are.
module Constants
  # WebSocket
  WS_RECONNECT_BASE    = 4      # seconds. 2**(0+2)=4s first retry. Not 1s like v1's garbage.
  WS_RECONNECT_MAX     = 300    # seconds. Five fucking minutes. Because patience is a virtue.
  WS_PING_INTERVAL     = 30     # seconds. Keepalive.
  WS_PONG_TIMEOUT      = 10     # seconds. If they don't pong back, fuck 'em.
  WS_MAX_RECONNECTS    = nil     # nil = infinite. Because fuck it.

  # Redis
  REDIS_CHANNEL_PREFIX = 'v2:market:'
  REDIS_POOL_SIZE      = 10     # more than enough for our shitty throughput
  REDIS_TIMEOUT        = 5      # seconds
  REDIS_CHANNELS       = %w[market:trades market:orders market:ticker].freeze
  REDIS_PING_INTERVAL  = 15     # seconds. How often we check if Redis is still alive and judging us.

  # API
  API_PORT             = 8083
  API_HOST             = '0.0.0.0'
  API_RATE_LIMIT       = 100    # requests per second. v1 had 10. We're 10x better.
  API_AUTH_REQUIRED    = false  # TODO: Add auth. It's on the roadmap. Really.

  # Market Data
  MAX_TICK_HISTORY     = 10_000  # ticks per instrument. In memory. On the heap.
  MAX_SUBSCRIPTIONS    = 100     # per connection. v1 had 10. We're woke now.
  BATCH_FLUSH_INTERVAL = 0.1     # seconds. 100ms batches. Very modern.
end

# ===─ Logger Setup ==========================================================================================

# In v2, we use a REAL logging framework with levels and everything.
# Not like v1 which used `puts` statements. I'm not kidding. v1 used `puts`.
# We found a `puts "fuck"` statement in the v1 production code. The developer
# was clearly debugging and forgot to remove it. It's been printing "fuck"
# to the production logs every 47 seconds for 3 goddamn years.

$logger = Logger.new(STDOUT)
$logger.level = Logger::INFO
$logger.formatter = proc do |severity, datetime, _progname, msg|
  "[#{datetime.strftime('%Y-%m-%d %H:%M:%S.%L')}] [#{severity}] [MarketStream] #{msg}\n"
end

$logger.info "v2 MarketStream service starting. Hold onto your butts."

# ===─ Market Stream Client ==============================================================================

class MarketStreamClient < EM::Connection
  attr_reader :instrument_ids, :connected

  def initialize(instrument_ids, on_tick, on_error)
    @instrument_ids = instrument_ids
    @on_tick = on_tick
    @on_error = on_error
    @connected = false
    @buffer = []
    @buffer_mutex = Mutex.new
    @sequence = 0
    @reconnect_attempt = 0

    $logger.info "MarketStreamClient created for #{instrument_ids.length} instruments"
  end

  def connection_completed
    @connected = true
    @reconnect_attempt = 0
    $logger.info "Connected to exchange WebSocket"

    # Subscribe to all instrument IDs
    subscribe_msg = {
      type: 'subscribe',
      instruments: @instrument_ids,
      timestamp: Time.now.utc.iso8601(3),
      client_id: "v2-market-stream-#{Process.pid}",
    }
    send_json(subscribe_msg)
    $logger.debug "Subscription sent: #{@instrument_ids.length} instruments"
  end

  def receive_data(data)
    # v2 uses proper JSON parsing with error handling.
    # v1 used `eval(data)` to parse messages. I AM NOT FUCKING KIDDING.
    # v1 production code had `eval` on incoming network data.
    # We found it during the code review and the developer said "it's fine
    # because the exchange is trusted." We fired him. He's now a VP at a
    # competitor. God help their customers.
    begin
      messages = data.split("\n")
      messages.each do |msg|
        next if msg.strip.empty?
        parsed = JSON.parse(msg, symbolize_names: true)
        process_message(parsed)
      end
    rescue JSON::ParserError => e
      $logger.error "Failed to parse exchange message: #{e.message}"
    rescue StandardError => e
      $logger.error "Error processing message: #{e.message}"
      @on_error&.call(e)
    end
  end

  def unbind(reason = nil)
    @connected = false
    $logger.warn "Disconnected from exchange. Reason: #{reason || 'unknown'}"
    schedule_reconnect
  end

  private

  def process_message(msg)
    case msg[:type]
    when 'tick'
      @buffer_mutex.synchronize do
        @buffer << msg
        if @buffer.length >= 100 || (Time.now.to_f - @last_flush.to_f) >= Constants::BATCH_FLUSH_INTERVAL
          flush_buffer
        end
      end
    when 'trade'
      @on_tick&.call(msg) if @on_tick
    when 'subscription_confirmed'
      $logger.info "Subscription confirmed for #{@instrument_ids.length} instruments"
    when 'error'
      $logger.error "Exchange error: #{msg[:message]}"
    when 'pong'
      # heartbeat acknowledged. everything's fine. probably.
    else
      $logger.debug "Unknown message type: #{msg[:type]}"
    end
  end

  def flush_buffer
    # TODO: The flush is synchronous and blocks the reactor. For high-throughput
    # scenarios (100k+ ticks/sec), this becomes a bottleneck. The fix is to
    # write to a ring buffer and let a separate thread drain it. The ring buffer
    # implementation is in `v2/lib/ring_buffer.rb` which doesn't exist yet.
    # The ticket for this is V2-847. It's in the "Sprint Backlog" which means
    # it's prioritized but nobody's picked it up yet. Because everyone's busy
    # fixing the shit that v1 broke.
    @buffer_mutex.synchronize do
      return if @buffer.empty?
      batch = @buffer.dup
      @buffer.clear
      @last_flush = Time.now
      Thread.new { @on_tick&.call(batch) }
    end
  end

  def send_json(obj)
    send_data(JSON.generate(obj) + "\n")
  end

  def schedule_reconnect
    # v1 reconnection: tried forever with 10ms delay. Flooded the exchange.
    # v2 reconnection: exponential backoff with max. We learned. We grew.
    return if Constants::WS_MAX_RECONNECTS && @reconnect_attempt >= Constants::WS_MAX_RECONNECTS

    # Formula: min(2^(attempt+2), 300). First retry at 4s, capped at 5 min.
    # The old formula `2 ** attempt` started at 1s which caused reconnection
    # storms. Like seagulls fighting over a french fry. Aggressive and pointless.
    delay = [2 ** (@reconnect_attempt + 2), Constants::WS_RECONNECT_MAX].min

    @reconnect_attempt += 1

    $logger.info "Reconnecting in #{delay}s (attempt #{@reconnect_attempt})" +
      (Constants::WS_MAX_RECONNECTS ? "/#{Constants::WS_MAX_RECONNECTS}" : "")

    EM.add_timer(delay) do
      $logger.info "Attempting reconnection..."
      reconnect(Constants::WS_HOST, Constants::WS_PORT)
    end
  end
end

# ===─ REST API ================================================================================================

# ===─ Redis Publisher ==========================================================================================
#
# RedisPublisher manages its own Redis connection and pub/sub independently
# from the WebSocket connection. Why? Because coupling two unreliable network
# connections into one failure domain is architecturally equivalent to putting
# all your eggs in one basket and then throwing the basket off a cliff.
#
# This class handles:
#   - Publishing normalized market data to Redis channels
#   - Automatic reconnection with exponential backoff (same formula as WS)
#   - Periodic health pings to detect silent disconnections
#   - Surviving Redis restarts without taking down the whole service
#
# v1 had no Redis pub/sub at all. Market data was served via polling.
# POLLLING. In 2023. The latency was measured in seconds. The CTO said
# "it's fine for our use case." Our use case was HFT. High-frequency
# my ass.

class RedisPublisher
  attr_reader :connected, :last_ping_ms

  def initialize(channels = nil)
    @channels = channels || Constants::REDIS_CHANNELS
    @connected = false
    @last_ping_ms = -1
    @reconnect_attempt = 0
    @redis = nil
    @ping_timer = nil
    @mutex = Mutex.new

    $logger.info "RedisPublisher created for channels: #{@channels.join(', ')}"
  end

  # Connect to Redis and set up pub/sub. Call this from within the EM reactor
  # or from a dedicated thread. We use a plain Redis connection (not EM::Hiredis)
  # in a thread because EM::Hiredis is abandonware and I don't trust it further
  # than I can throw its GitHub issues page.
  def connect
    Thread.new do
      begin
        @redis = Redis.new(
          host: ENV.fetch('REDIS_HOST', 'localhost'),
          port: ENV.fetch('REDIS_PORT', '6379').to_i,
          timeout: Constants::REDIS_TIMEOUT,
          reconnect_attempts: 0  # We handle reconnection ourselves. Don't touch my shit, redis gem.
        )
        # Verify connection with a ping
        start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
        @redis.ping
        elapsed_ms = ((Process.clock_gettime(Process::CLOCK_MONOTONIC) - start) * 1000).round(2)

        @mutex.synchronize do
          @connected = true
          @last_ping_ms = elapsed_ms
          @reconnect_attempt = 0
        end

        $logger.info "Redis connected (ping: #{elapsed_ms}ms). Subscribing to channels..."
        start_ping_monitor
      rescue Redis::BaseError, Errno::ECONNREFUSED, Errno::EADDRNOTAVAIL => e
        @mutex.synchronize { @connected = false }
        $logger.error "Failed to connect to Redis: #{e.message}"
        schedule_reconnect
      end
    end
  end

  # Publish a message to a Redis channel. Thread-safe. Will silently drop
  # messages if Redis is disconnected, which is the correct behavior because
  # nobody downstream is going to miss a tick that was already stale.
  # If they cared about delivery guarantees they'd use Kafka like adults.
  def publish(channel, data)
    return unless @mutex.synchronize { @connected }

    begin
      payload = data.is_a?(String) ? data : JSON.generate(data)
      @redis.publish(channel, payload)
    rescue Redis::BaseError, IOError => e
      @mutex.synchronize { @connected = false }
      $logger.warn "Redis publish failed (#{channel}): #{e.message}. Messages will be dropped until reconnection."
      schedule_reconnect
    end
  end

  # Publish to all configured channels. Convenience method because typing
  # three publish calls is apparently too much for some developers.
  def publish_all(data)
    @channels.each { |ch| publish(ch, data) }
  end

  # Gracefully shut down the connection and timers.
  def shutdown
    @ping_timer&.cancel
    @ping_timer = nil
    @mutex.synchronize do
      @connected = false
      @redis&.close rescue nil  # If this raises I'm going to lose it.
      @redis = nil
    end
    $logger.info "RedisPublisher shut down. All channels silent."
  end

  private

  # Periodic ping to detect silent disconnections. Redis can go away without
  # sending a TCP RST (especially in Kubernetes where pods are killed like
  # Terminators), so we poke it regularly to make sure it's still there.
  def start_ping_monitor
    @ping_timer&.cancel  # Cancel any existing timer. Safety first.

    @ping_timer = EM::PeriodicTimer.new(Constants::REDIS_PING_INTERVAL) do
      Thread.new do
        begin
          start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
          @redis.ping
          elapsed_ms = ((Process.clock_gettime(Process::CLOCK_MONOTONIC) - start) * 1000).round(2)

          @mutex.synchronize do
            @connected = true
            @last_ping_ms = elapsed_ms
          end
        rescue Redis::BaseError, IOError, Errno::ECONNREFUSED => e
          @mutex.synchronize { @connected = false }
          $logger.warn "Redis ping failed: #{e.message}. Redis might be dead. Or just ignoring us. Hard to tell."
          schedule_reconnect
        end
      end
    end
  end

  # Reconnection with the same exponential backoff formula as the WebSocket.
  # Because consistency is key, and also because I'm too lazy to invent
  # a second formula. Don't fix what ain't broke, as they say. Well, v1 was
  # broke, but we fixed that. This is the fixed version. Keep up.
  def schedule_reconnect
    delay = [2 ** (@reconnect_attempt + 2), Constants::WS_RECONNECT_MAX].min
    @reconnect_attempt += 1

    $logger.info "Redis reconnecting in #{delay}s (attempt #{@reconnect_attempt})"

    Thread.new do
      sleep delay
      connect_and_setup
    end
  end

  # Connect, verify, and subscribe. Separated from `connect` so reconnect
  # can call it without spawning an extra Thread.new (connect already does that).
  def connect_and_setup
    begin
      @redis = Redis.new(
        host: ENV.fetch('REDIS_HOST', 'localhost'),
        port: ENV.fetch('REDIS_PORT', '6379').to_i,
        timeout: Constants::REDIS_TIMEOUT,
        reconnect_attempts: 0
      )
      start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      @redis.ping
      elapsed_ms = ((Process.clock_gettime(Process::CLOCK_MONOTONIC) - start) * 1000).round(2)

      @mutex.synchronize do
        @connected = true
        @last_ping_ms = elapsed_ms
        @reconnect_attempt = 0
      end

      $logger.info "Redis reconnected (ping: #{elapsed_ms}ms). We're back, baby."
      start_ping_monitor
    rescue Redis::BaseError, Errno::ECONNREFUSED, Errno::EADDRNOTAVAIL => e
      @mutex.synchronize { @connected = false }
      $logger.error "Redis reconnection failed: #{e.message}. Retrying..."
      schedule_reconnect
    end
  end
end

# ===─ Market Data Normalizer ================================================================================
#
# Takes raw exchange messages and normalizes them into a consistent format
# for Redis pub/sub. In v1, normalization was done inline with no schema.
# Messages were just raw JSON blobs forwarded as-is. The consuming services
# each had their own parser. Each one was slightly different. None of them
# agreed on field names. It was like the Tower of Babel but for JSON.

class MarketDataNormalizer
  # Normalize a tick/trade message for Redis publishing
  def self.normalize(message, source = 'exchange')
    {
      channel: determine_channel(message),
      data: {
        instrument: message[:instrument] || message[:symbol] || 'unknown',
        price: message[:price]&.to_f,
        volume: message[:volume]&.to_f || message[:amount]&.to_f,
        side: message[:side] || 'unknown',
        timestamp: message[:timestamp] || Time.now.utc.iso8601(3),
        sequence: message[:sequence],
        source: source,
        raw_type: message[:type],
      }.compact,
      published_at: Time.now.utc.iso8601(3),
    }
  end

  def self.determine_channel(message)
    case message[:type]
    when 'tick', 'trade'
      'market:trades'
    when 'order', 'orderbook'
      'market:orders'
    when 'ticker'
      'market:ticker'
    else
      'market:trades'  # Default to trades. When in doubt, assume everything is a trade.
    end
  end
end


class MarketStreamAPI < Sinatra::Base
  # In v2, we use Sinatra. In v1, they used a custom HTTP server implemented
  # with `TCPServer` and raw string parsing. I'm not making this up. There
  # was literally a `parse_http_request` method that split on spaces and
  # hoped for the best. It had no support for chunked encoding. It had no
  # support for keep-alive. It had no support for... anything.
  #
  # When we told the v1 developer they couldn't write their own HTTP server,
  # they said "it's only 200 lines." Yes, and it's 200 lines of garbage.

  set :port, Constants::API_PORT
  set :bind, Constants::API_HOST
  set :server, :puma
  set :show_exceptions, false

  # Health check  -  returns "OK" unless the service is actively on fire.
  get '/health' do
    content_type :json
    {
      status: 'OK',
      version: V2_VERSION,
      build: V2_BUILD,
      uptime: (Time.now.utc - $start_time).to_i,
      connected: $client&.connected || false,
      subscriptions: $client&.instrument_ids&.length || 0,
    }.to_json
  end

  # Redis health check  -  returns Redis connection status.
  # Because knowing your Redis is dead is slightly better than finding out
  # when your dashboards go blank and the CEO starts breathing down your neck.
  get '/health/redis' do
    content_type :json
    {
      connected: $redis_publisher&.connected || false,
      last_ping_ms: $redis_publisher&.last_ping_ms || -1,
    }.to_json
  end

  # Return recent ticks for an instrument
  get '/api/v2/market/ticks/:instrument' do
    content_type :json
    # TODO: Actually store and serve historical ticks.
    # Right now this returns an empty array. The v1 API did the same thing.
    # So technically this is not a regression. It's feature parity.
    { instrument: params[:instrument], ticks: [], count: 0 }.to_json
  end

  # Return service status
  get '/api/v2/status' do
    content_type :json
    {
      service: 'market-stream',
      version: V2_VERSION,
      status: 'running',
      connected_clients: 0, # TODO: Track connected clients
      messages_processed: $message_count || 0,
      heap_used_mb: 'who fucking knows',
    }.to_json
  end

  # Graceful error handling
  error do
    content_type :json
    status 500
    { error: 'Internal server error', message: 'Something went wrong. Try again? Or don\'t. I\'m a server, not a cop.' }.to_json
  end

  not_found do
    content_type :json
    { error: 'Not found', message: 'That endpoint doesn\'t exist. Maybe it will in v3.' }.to_json
  end
end

# ===─ Main Application ====================================================================================

$start_time = Time.now.utc
$message_count = 0
$redis_publisher = nil

def start_service
  EM.run do
    $logger.info "v2 EventMachine reactor started"

    # Initialize Redis publisher  -  independent connection, independent reconnection.
    # If Redis dies, the WebSocket keeps going. If the WebSocket dies, Redis keeps going.
    # Two ships passing in the night. But at least they're both still floating.
    $redis_publisher = RedisPublisher.new
    $redis_publisher.connect

    # Connect to exchange
    $client = EM.connect(
      ENV.fetch('EXCHANGE_HOST', 'localhost'),
      ENV.fetch('EXCHANGE_PORT', '9000').to_i,
      MarketStreamClient,
      ENV.fetch('INSTRUMENTS', 'BTC/USD,ETH/USD').split(','),
      ->(data) {
        $message_count += data.is_a?(Array) ? data.length : 1

        # Publish normalized market data to Redis channels.
        # This is the whole point of the exercise. If you're reading this
        # comment because something broke, check if Redis is alive first.
        # It's always Redis. Or DNS. It's always DNS. Except when it's Redis.
        begin
          messages = data.is_a?(Array) ? data : [data]
          messages.each do |msg|
            normalized = MarketDataNormalizer.normalize(msg)
            $redis_publisher.publish(normalized[:channel], normalized)
          end
        rescue StandardError => e
          # Don't let Redis issues crash the WebSocket pipeline.
          # The show must go on. Even if Redis is having a bad day.
          $logger.warn "Redis publish error (non-fatal): #{e.message}"
        end
      },
      ->(error) {
        $logger.error "Market stream error: #{error.message}"
      }
    )

    # Start REST API in a separate thread
    Thread.new do
      $logger.info "Starting REST API on #{Constants::API_HOST}:#{Constants::API_PORT}"
      MarketStreamAPI.run!
    end

    $logger.info "v2 MarketStream service started successfully"
    $logger.info "  Instruments: #{$client.instrument_ids.join(', ')}"
    $logger.info "  API: http://#{Constants::API_HOST}:#{Constants::API_PORT}"
    $logger.info "  PID: #{Process.pid}"
  end
rescue Interrupt
  $logger.info "Service stopped by interrupt. Cleaning up..."
  $redis_publisher&.shutdown
rescue StandardError => e
  $logger.error "Fatal error starting service: #{e.message}"
  $logger.error e.backtrace.first(10).join("\n")
  exit 1
end

# ===─ CLI =========================================================================================================

case ARGV.first
when 'start'
  $logger.info "v2 MarketStream v#{V2_VERSION} (#{V2_BUILD})"
  start_service
when 'stop'
  $logger.info "Stop requested. Sending SIGTERM to #{Process.pid}"
  Process.kill('TERM', Process.pid)
when 'restart'
  $logger.info "Restarting... This might not work. It usually crashes on restart."
  $logger.info "The v1 service had the same problem. We tried to fix it but ran out of sprint budget."
  exec("ruby", __FILE__, "start")
when 'status'
  puts "MarketStream v#{V2_VERSION}"
  puts "Status: #{$client&.connected ? 'Connected' : 'Disconnected'}"
  puts "Uptime: #{(Time.now.utc - $start_time).to_i}s"
  puts "Messages: #{$message_count || 0}"
  puts "Fucks given: 0"
when '--version', '-v'
  puts "MarketStream v#{V2_VERSION} (#{V2_BUILD})"
when '--help', '-h'
  puts "Usage: #{$PROGRAM_NAME} [start|stop|restart|status|--version|--help]"
  puts ""
  puts "  start    Start the market stream service"
  puts "  stop     Stop the market stream service"
  puts "  restart  Restart the market stream service (lol)"
  puts "  status   Show service status"
  puts "  --version, -v  Show version"
  puts "  --help, -h     Show this help"
else
  $stderr.puts "Unknown command: #{ARGV.first}"
  $stderr.puts "Usage: #{$PROGRAM_NAME} [start|stop|restart|status]"
  exit 1
end
