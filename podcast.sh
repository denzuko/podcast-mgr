#!/bin/tcsh
##
# Podcast sync script
# version 1.1
# Copyright (C)2026 Dwight Spencer <denzuko@dapla.net>. All Rights Reserved.
# Licenced for use and distribution under the BSD 2-clause licence.
##
# Reads $XDG_CONFIG_HOME/podcasts/feeds.xml (managed by index.cgi) and
# downloads podcast episodes according to the schedule defined therein.
# Intended to be run as a cron job; typically at midnight.
##

# --- Configuration ---
set CONFIG_DIR = "$HOME/.config/podcasts"
if ( $?XDG_CONFIG_HOME ) then
    set CONFIG_DIR = "$XDG_CONFIG_HOME/podcasts"
endif
set DEST_DIR = "$HOME/Downloads/Podcasts"
if ( $?XDG_DOWNLOAD_DIR ) then
    set DEST_DIR = "$XDG_DOWNLOAD_DIR/Podcasts"
endif
set CONFIG_FILE   = "$CONFIG_DIR/feeds.xml"
set CONFIG_SCHEMA = "$CONFIG_DIR/feeds.xsd"
set CONFIG_STYLE  = "$CONFIG_DIR/feeds.xsl"
set PLAYLIST_FILE = "$DEST_DIR/podcasts.m3u"
set AGENT = "MetisAI/1.0 (MetisAi/PodcastFeed +https://3umgroup.com/ai/)"

# --- Flags ---
set RECENT_ONLY  = 0
set MAKE_PLAYLIST = 0
set ONLY_THIS    = ""
set FORCE        = 0
set DEBUG        = 0
set VERBOSE      = 0

# --- Time ---
set CUR_DAY  = `date +%a`
set CUR_HOUR = `date +%H`
set CUR_MIN  = `date +%M`

# --- Arguments ---
while ( $#argv > 0 )
    switch ( $1 )
        case "-h":
        case "--help":
            goto usage
        case "-r":
        case "--recent":
            set RECENT_ONLY = 1
            breaksw
        case "-p":
        case "--playlist":
            set MAKE_PLAYLIST = 1
            breaksw
        case "-f":
        case "--force":
            set FORCE = 1
            breaksw
        case "-d":
        case "--debug":
            set DEBUG = 1
            breaksw
        case "-v":
        case "--verbose":
            set VERBOSE = 1
            breaksw
        case "-t":
        case "--title":
            shift
            if ( ! $#argv ) then
                echo "Error: --title requires an argument."
                goto usage
            endif
            set ONLY_THIS = $1
            breaksw
        case "-a":
        case "--agent":
            shift
            if ( ! $#argv ) then
                echo "Error: --agent requires an argument."
                goto usage
            endif
            set AGENT = $1
            breaksw
        default:
            echo "Unknown option: $1"
            goto usage
    endsw
    shift
end

# --- Main ---
if ( 1 == $DEBUG ) then
    unset notify
    unset printexitvalue
    set verbose
endif

onintr finish

alias lint 'set _tmp = `mktemp`; \
    xmlstarlet sel -N xs="http://www.w3.org/2001/XMLSchema" -t -c "//xs:schema" \!:1 > $_tmp && \
    xmlstarlet val -e -s $_tmp \!:1; \
    rm -f $_tmp; \
    unset _tmp'

if ( ! -d "$CONFIG_DIR" ) mkdir -p "$CONFIG_DIR"

if ( ! -f "$CONFIG_FILE" ) tee "$CONFIG_FILE" >/dev/null << EOL
<?xml version="1.0" encoding="UTF-8"?>
<?xml-stylesheet type="text/xsl" href="feeds.xsl"?>
<subscriptions>
  <podcast title="Daily News" url="https://feeds.npr.org/510289/podcast.xml" scope="latest" day="Daily" pull_time="06" />
  <podcast title="Weekly Tech" url="https://lexfridman.com/feed/podcast/" scope="all" day="Tue" pull_time="11" />
  <podcast title="2600 Off The Hook" url="https://2600.com/oth-broadband.xml" scope="latest" day="Tue" pull_time="18" />
  <podcast title="2600 Off The Wall" url="https://2600.com/otw-broadband.xml" scope="latest" day="Wed" pull_time="18" />
</subscriptions>
EOL

if ( ! -f "$CONFIG_SCHEMA" ) tee "$CONFIG_SCHEMA" > /dev/null << EOL
<?xml version="1.0" encoding="UTF-8"?>
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:element name="subscriptions">
    <xs:complexType>
      <xs:sequence>
        <xs:element name="podcast" maxOccurs="unbounded" minOccurs="1">
          <xs:complexType>
            <xs:attribute name="title" type="xs:string" use="required" />
            <xs:attribute name="scope">
              <xs:simpleType>
                <xs:restriction base="xs:string">
                  <xs:enumeration value="all"/>
                  <xs:enumeration value="latest"/>
                  <xs:enumeration value="none"/>
                </xs:restriction>
              </xs:simpleType>
            </xs:attribute>
            <xs:attribute name="url" type="xs:anyURI" use="required" />
            <xs:attribute name="day">
              <xs:simpleType>
                <xs:restriction base="xs:string">
                  <xs:enumeration value="Daily"/>
                  <xs:enumeration value="Mon"/>
                  <xs:enumeration value="Tue"/>
                  <xs:enumeration value="Wed"/>
                  <xs:enumeration value="Thu"/>
                  <xs:enumeration value="Fri"/>
                  <xs:enumeration value="Sat"/>
                  <xs:enumeration value="Sun"/>
                </xs:restriction>
              </xs:simpleType>
            </xs:attribute>
            <xs:attribute name="pull_time">
              <xs:simpleType>
                <xs:restriction base="xs:string">
                  <xs:enumeration value="00"/>
                  <xs:enumeration value="01"/>
                  <xs:enumeration value="02"/>
                  <xs:enumeration value="03"/>
                  <xs:enumeration value="04"/>
                  <xs:enumeration value="05"/>
                  <xs:enumeration value="06"/>
                  <xs:enumeration value="07"/>
                  <xs:enumeration value="08"/>
                  <xs:enumeration value="09"/>
                  <xs:enumeration value="10"/>
                  <xs:enumeration value="11"/>
                  <xs:enumeration value="12"/>
                  <xs:enumeration value="13"/>
                  <xs:enumeration value="14"/>
                  <xs:enumeration value="15"/>
                  <xs:enumeration value="16"/>
                  <xs:enumeration value="17"/>
                  <xs:enumeration value="18"/>
                  <xs:enumeration value="19"/>
                  <xs:enumeration value="20"/>
                  <xs:enumeration value="21"/>
                  <xs:enumeration value="22"/>
                  <xs:enumeration value="23"/>
                </xs:restriction>
              </xs:simpleType>
            </xs:attribute>
          </xs:complexType>
        </xs:element>
      </xs:sequence>
    </xs:complexType>
  </xs:element>
</xs:schema>
EOL

if ( ! -f "$CONFIG_STYLE" ) tee "$CONFIG_STYLE" >/dev/null << EOL
<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
<xsl:template match="/">
<html>
 <body style="font-family:sans-serif; background:#f4f4f4; padding:20px;">
   <h2>Podcast Subscriptions</h2>
   <table border="1" style="border-collapse:collapse; width:100%; background:white;">
     <tr style="background:#ddd;">
       <th style="padding:10px;">Title</th>
       <th>Schedule (Day @ Time)</th>
       <th>RSS URL</th>
     </tr>
     <xsl:for-each select="subscriptions/podcast">
     <tr>
       <td style="padding:10px;"><xsl:value-of select="@title"/></td>
       <td style="text-align:center;"><xsl:value-of select="@day"/> @ <xsl:value-of select="@pull_time"/></td>
       <td><a href="{@url}"><xsl:value-of select="@url"/></a></td>
     </tr>
     </xsl:for-each>
   </table>
 </body>
 </html>
</xsl:template>
</xsl:stylesheet>
EOL

if ( ! -d "$DEST_DIR" ) mkdir -p "$DEST_DIR"

xmlstarlet val -e -s $CONFIG_SCHEMA $CONFIG_FILE
if ( 0 != $status ) then
    if ( 1 == $VERBOSE ) echo "Error processing config file"
    kill -TERM $$ >/dev/null 2>&1
endif

if ( 1 == $VERBOSE ) echo "Current System Time: $CUR_DAY at ${CUR_HOUR}:${CUR_MIN}"

set XPATH_FILTER = "//podcast[(@day='Daily' or @day='$CUR_DAY') and @pull_time<='$CUR_HOUR']/@url"
if ( 1 == $FORCE ) set XPATH_FILTER = "//podcast/@url"
if (($?ONLY_THIS) && ("" != "$ONLY_THIS" )) then
    set XPATH_FILTER = "//podcast[(@day='Daily' or @day='$CUR_DAY') and @title='$ONLY_THIS' and @pull_time<='$CUR_HOUR']/@url"
    if ( 1 == $FORCE ) set XPATH_FILTER = "//podcast[@title='$ONLY_THIS']/@url"
endif

set FEEDS = `xmlstarlet sel -t -v "$XPATH_FILTER" "$CONFIG_FILE"`
if ( "" == "$FEEDS" ) then
    if ( 1 == $VERBOSE ) echo "No podcasts scheduled for this time slot."
    kill -TERM $$ >/dev/null 2>&1
endif

set temp_files = ()
foreach feed ( $FEEDS:q )
    set title = `xmlstarlet sel -t -v "//podcast[@url='$feed']/@title" "$CONFIG_FILE"`
    if (0 != $status) then
        if ( 1 == $VERBOSE ) echo "Error parsing config: $status"
        continue
    endif
    set scope = `xmlstarlet sel -t -v "//podcast[@url='$feed']/@scope" "$CONFIG_FILE"`
    if (0 != $status) then
        if ( 1 == $VERBOSE ) echo "Error parsing config: $status"
        continue
    endif
    if ( 1 == $VERBOSE ) echo ">>> Syncing: $title"
    switch ($scope:q)
        case "none":
            continue
            breaksw
        case "all":
            set ep_xpath = "//enclosure/@url"
            breaksw
        case "latest":
        default:
            set ep_xpath = "(//enclosure)[1]/@url"
            breaksw
    endsw
    if ( 1 == $RECENT_ONLY ) set ep_xpath = "(//enclosure)[1]/@url"

    set tmp_xml = `mktemp`
    set temp_files = ( $temp_files $tmp_xml )
    if ( 1 == $DEBUG ) echo $tmp_xml
    curl -A "$AGENT" -sLk "$feed" -o "$tmp_xml"
    if (0 != $status) then
        if ( 1 == $VERBOSE ) echo "Error retrieving feed: $feed:q"
        continue
    endif
    set mp3_urls = `xmlstarlet sel -t -v "$ep_xpath" "$tmp_xml" | sed 's/\(&\)\(amp\);/\1/g'`
    if ("" == "$mp3_urls" ) then
        if ( 1 == $VERBOSE ) echo "No urls found from rss feed"
        continue
    endif

    # Extract podcast-level metadata for ID3 tagging
    set pod_album  = `xmlstarlet sel -t -v "//channel/title" "$tmp_xml" 2>/dev/null || echo "$title"`
    set pod_artist = `xmlstarlet sel -t -v "//channel/itunes:author" "$tmp_xml" 2>/dev/null`
    if ("" == "$pod_artist") set pod_artist = `xmlstarlet sel -t -v "//channel/author" "$tmp_xml" 2>/dev/null`
    if ("" == "$pod_artist") set pod_artist = "$title"

    set ep_idx = 1
    foreach mp3_url ( $mp3_urls:q )
        set filename = `echo "$mp3_url" | cut -d'?' -f1 | xargs basename`
        set safe_title = `echo "$title" | tr ' ' '_' | tr -cd '[:alnum:]_-'`
        set filename = `date +${safe_title}_%Y-%m-%d_${filename}`
        if ( ! -f "$DEST_DIR/$filename" ) then
            if ( 1 == $VERBOSE ) echo "  Downloading: $filename"
            curl -A "$AGENT" -sLk "$mp3_url" -o "$DEST_DIR/$filename"
            if (0 != $status) then
                if ( 1 == $VERBOSE ) echo "Error retrieving audio file: $mp3_url"
                @ ep_idx++
                continue
            endif

            # Tag with ID3 metadata from RSS feed (requires id3v2)
            if ( -x "`which id3v2 2>/dev/null`" ) then
                set ep_title = `xmlstarlet sel -t -v "(//item)[$ep_idx]/title" "$tmp_xml" 2>/dev/null`
                set ep_year  = `xmlstarlet sel -t -v "(//item)[$ep_idx]/pubDate" "$tmp_xml" 2>/dev/null | grep -oE '[0-9]{4}' | head -1`
                if ("" == "$ep_title") set ep_title = "$filename:r"
                if ("" == "$ep_year")  set ep_year  = `date +%Y`
                if ( 1 == $VERBOSE ) echo "  Tagging: $ep_title"
                id3v2 \
                    --artist  "$pod_artist" \
                    --album   "$pod_album"  \
                    --song    "$ep_title"   \
                    --year    "$ep_year"    \
                    "$DEST_DIR/$filename" >& /dev/null
            endif
        endif
        @ ep_idx++
    end
end

if ( 1 == $MAKE_PLAYLIST ) goto playlist
exec kill -TERM $$ >& /dev/null

playlist:
if ( 1 == $VERBOSE ) echo "Updating playlist: $PLAYLIST_FILE"
echo "#EXTM3U" > "$PLAYLIST_FILE"
find "$DEST_DIR" -maxdepth 1 -name "*.mp3" -printf "%T@ %p\n" | awk -v out="$PLAYLIST_FILE" '\
    { a[$1] = substr($0, index($0,$2)) } \
    END { n = asorti(a, b, "@ind_num_asc"); for (i=1; i<=n; i++) print a[b[i]] >> out}'

finish:
foreach tmpfile ( $temp_files )
    if ( -f $tmpfile ) rm -f $tmpfile
end
exit 0

usage:
cat - << EOL
Usage: $0 [options]
Options:
    -r, --recent        Download recent episode only [overloads scope]
    -p, --playlist      Create a m3u playlist
    -t, --title         Pull only this podcast (requires title)
    -f, --force         Ignore scheduled time
    -a, --agent         Override user-agent string
    -d, --debug         Enable debug output
    -v, --verbose       Enable verbose output
    -h, --help          Show this message

Dependencies:
    curl, xmlstarlet    Required
    id3v2               Optional — install for automatic ID3 tag updates
                        Debian/Ubuntu: apt install id3v2
                        FreeBSD: pkg install id3v2
EOL
exit 1
