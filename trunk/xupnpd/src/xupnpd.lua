cfg={}

-- multicast interface for SSDP exchange, 'eth0', 'br0', 'br-lan' for example
cfg.ssdp_interface='lo'

-- 'cfg.ssdp_loop' enables multicast loop (if player and server in one host)
cfg.ssdp_loop=1

-- HTTP port for incoming connections
cfg.http_port=4044

-- syslog facility (syslog,local0-local7)
cfg.log_facility='local0'

-- 'cfg.daemon' detach server from terminal
cfg.daemon=false

-- silent mode - no logs, no pid file
cfg.embedded=false

-- 'cfg.debug' enables SSDP debug output to stdout (if cfg.daemon=false)
-- 0-off, 1-basic, 2-messages
cfg.debug=1

-- 'udpxy' url for multicast playlists (udp://@...)
cfg.udpxy_url='http://192.168.1.1:4022'

-- 'cfg.proxy' enables proxy for injection DLNA headers to stream
-- 0-off, 1-radio, 2-radio/TV
cfg.proxy=2

-- I/O timeout
cfg.http_timeout=15

-- 'cfg.dlna_extras' enables DLNA extras
cfg.dlna_extras=true

-- enables UPnP/DLNA notify when reload playlist
cfg.dlna_notify=true

-- group by 'group-title'
cfg.group=true

-- Device name
cfg.name='UPnP-IPTV'

-- static device UUID, '60bd2fb3-dabe-cb14-c766-0e319b54c29a' for example or nil
cfg.uuid='60bd2fb3-dabe-cb14-c766-0e319b54c29a'

-- max url cache size
cfg.cache_size=8

-- url cache item ttl (sec)
cfg.cache_ttl=900

-- feeds update interval (seconds, 0 - disabled)
cfg.feeds_update_interval=0

-- playlist (m3u file path or path with alias
playlist=
{
    { './playlists/mozhay.m3u',             'Mozhay.tv' },
--    { './localmedia', 'Local Media Files', '127.0.0.1;192.168.1.1' }
}

-- feeds list (plugin, feed name, feed type)
feeds=
{
    { 'vimeo',   'channel/hd',   'Vimeo HD Channel' },
    { 'vimeo',   'channel/hdxs', 'HD Xtreme sports' },
    { 'vimeo',   'channel/mtb',  'Mountain Bike Channel' },
    { 'youtube', 'top_rated',    'YouTube Top Rated' },
}

-- log ident, pid file end www root
cfg.version='1.0-rc1'
cfg.log_ident=arg[1] or 'xupnpd'
cfg.pid_file='/var/run/'..cfg.log_ident..'.pid'
cfg.www_root='./www/'
cfg.tmp_path='/tmp/'
cfg.plugin_path='./plugins/'
cfg.config_path='./config/'
cfg.playlists_path='./playlists/'
--cfg.feeds_path='/tmp/xupnpd-feeds/'
cfg.ui_path='./ui/'

dofile('xupnpd_main.lua')