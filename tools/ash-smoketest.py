import socket, sys, time

FLAG,ESC,XON,XOFF,SUB,CAN = 0x7E,0x7D,0x11,0x13,0x18,0x1A
RESERVED = {FLAG,ESC,XON,XOFF,SUB,CAN}

rand=[]
r=0x42
for _ in range(256):
    rand.append(r)
    r = ((r>>1)^0xB8) if (r&1) else (r>>1)

def crc(d):
    c=0xFFFF
    for b in d:
        c^=b<<8
        for _ in range(8):
            c=((c<<1)^0x1021)&0xFFFF if c&0x8000 else (c<<1)&0xFFFF
    return c

def frame(body):
    out=bytearray()
    full=bytes(body)+crc(body).to_bytes(2,'big')
    for b in full:
        if b in RESERVED: out+=bytes([ESC,b^0x20])
        else: out.append(b)
    out.append(FLAG)
    return bytes(out)

def readframe(s, timeout=8):
    s.settimeout(timeout)
    buf=bytearray(); esc=False
    while True:
        c=s.recv(1)
        if not c: return None
        b=c[0]
        if b in (XON,XOFF): continue
        if b==CAN: buf.clear(); esc=False; continue
        if b==ESC: esc=True; continue
        if b==FLAG:
            if len(buf)>=3:
                body=bytes(buf[:-2]); want=(buf[-2]<<8)|buf[-1]
                if crc(body)==want: return body
                print("  !! crc mismatch"); 
            buf.clear(); esc=False; continue
        if esc: b^=0x20; esc=False
        buf.append(b)

def mask(p): return bytes(b^rand[i] for i,b in enumerate(p))

host=sys.argv[1]; port=int(sys.argv[2])
s=socket.create_connection((host,port),10)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

print("-> RST")
s.sendall(bytes([CAN])+frame([0xC0]))
f=readframe(s)
print("<- %s" % f.hex(' '), "= RSTACK" if f and f[0]==0xC1 else "= UNEXPECTED")
if not f or f[0]!=0xC1: sys.exit(1)

frm=0
def send_ezsp(ez, label):
    global frm
    ctrl = (frm<<4) | 0  # ackNum tracked loosely; bridge acks us
    body = bytes([ctrl])+mask(ez)
    print("-> %s: ezsp %s" % (label, ez.hex(' ')))
    s.sendall(frame(body))
    frm=(frm+1)&7
    # collect until we get a DATA frame (ACKs are 0x8n)
    for _ in range(4):
        f=readframe(s)
        if f is None: return None
        print("<- raw frame: %s" % f.hex(' '))
        if (f[0]&0x80)==0x80:
            print("   (control frame ctrl=%02x)" % f[0]); continue
        payload = mask(f[1:])
        print("   DATA ezsp %s" % payload.hex(' '))
        return payload
    return None

# version(8), legacy 3-byte header
r1 = send_ezsp(bytes([0x00,0x00,0x00,0x08]), "version(8)")
if not r1: print("no version reply"); sys.exit(1)
ver = r1[3]
print("   => NCP EZSP v%d, stackType 0x%02X, stackVersion 0x%04X"
      % (ver, r1[4], r1[5]|(r1[6]<<8)))

# getEui64 (0x26) in the negotiated layout
if ver>=8: ez=bytes([0x01,0x00,0x01,0x26,0x00])
elif ver>=5: ez=bytes([0x01,0x00,0xFF,0x00,0x26])
else: ez=bytes([0x01,0x00,0x26])
r2 = send_ezsp(ez, "getEui64")
if r2:
    hdr = 5 if ver>=5 else 3
    eui = r2[hdr:hdr+8]
    print("   => EUI64 %s" % ':'.join('%02X'%b for b in reversed(eui)))

# getNetworkParameters (0x28)
if ver>=8: ez=bytes([0x02,0x00,0x01,0x28,0x00])
elif ver>=5: ez=bytes([0x02,0x00,0xFF,0x00,0x28])
else: ez=bytes([0x02,0x00,0x28])
r3 = send_ezsp(ez, "getNetworkParameters")
if r3:
    hdr = 5 if ver>=5 else 3
    st,nt = r3[hdr], r3[hdr+1]
    ext = r3[hdr+2:hdr+10]; pan = r3[hdr+10]|(r3[hdr+11]<<8); ch=r3[hdr+14]
    print("   => status 0x%02X nodeType %d panId 0x%04X channel %d extPan %s"
          % (st,nt,pan,ch,':'.join('%02X'%b for b in reversed(ext))))
s.close()
print("\nOK")
