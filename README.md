# Algaerithm — Build Your Own Pod

Algaerithm is a citizen-science river-monitoring project. A floating pod holds a
colony of microalgae (*Chlamydomonas*) in river water and watches how its colour
changes — greener water means a higher nutrient load. A small camera inside the
pod sends a photo to the cloud every few seconds, and those photos appear live on
the shared map at the project website.

This guide covers the **camera unit**: what to buy, how to flash it, and how to
get your pod's live image onto the shared Algaerithm map.

> One pod tells you about one stretch of water. A hundred pods start to tell you
> about a country. The more Guardians who join, the harder the data is to ignore.

---

## What you'll need

**Hardware**

- **Seeed Studio XIAO ESP32S3 Sense** — a tiny ESP32 board with a built-in
  camera and microSD slot. This is the board the reference pod uses.
  Search "XIAO ESP32S3 Sense" at the Seeed Studio store or any electronics
  retailer that stocks Seeed boards.
- A USB-C cable to flash and power it.
- (Optional) a microSD card if you also want to save full-resolution photos
  locally. The cloud upload works without one.
- A waterproof enclosure for the floating pod. Enclosure design is part of the
  wider project and not covered here.

**Software & accounts**

- The [Arduino IDE](https://www.arduino.cc/en/software) with ESP32 board support
  installed.
- A free [Adafruit IO](https://io.adafruit.com) account — this is the cloud
  service that receives your photos and makes them readable by the website.

---

## Step 1 — Set up Adafruit IO

Adafruit IO stores your latest photo in something called a **feed**. The website
reads that feed to show your image. You need two feeds.

1. Sign up at [io.adafruit.com](https://io.adafruit.com) (free).
2. Go to **Feeds → New Feed** and create two feeds. Name them simply:
   - `image-latest`
   - `last-photo-time`
3. **Check the feed _key_, not just the name.** Click into each feed and look at
   its **Key** (under the feed name or in its settings). The key is what the code
   and the website actually use. If you typed a dot or unusual characters in the
   name, Adafruit may turn them into something unexpected (for example a `.` can
   become `-dot-`). Make the keys clean: `image-latest` and `last-photo-time`.
4. Find your **username** (top-right account menu) and your **AIO Key**
   (the yellow key icon). You'll paste both into the code.

### ⚠️ Make both feeds Public — and keep them public

The website reads your feeds **without a password**, so they must be set to
**Public**. A public feed is still **read-only** to the world: anyone can see the
image, but only your AIO Key can upload to it. Your key stays private.

For each feed: open it → **Feed settings / privacy** → set **Public** → save.

**Confirm it worked.** Open this URL in a private/incognito window (logged out),
replacing the username and key with yours:

```
https://io.adafruit.com/api/v2/YOUR_USERNAME/feeds/image-latest
```

- If you see a block of text (JSON) including `"visibility":"public"` — you're
  good.
- If you see `{"error":"not found..."}` — the feed is still **private**. Set it
  to Public and check again.

> **This is the single most common reason a pod doesn't show up.** Adafruit's
> privacy setting can quietly flip back to private when you change other feed
> settings (especially toggling Feed History). After *any* change to a feed,
> re-check the incognito URL before relying on it.

### Turn Feed History off (recommended)

Your camera uploads constantly, and stored history fills your free quota fast.
In each feed's settings, turn **Feed History off** so it keeps only the latest
value. Then **re-check the Public setting** — toggling history can reset it.

---

## Step 2 — Flash the camera

1. Open the camera sketch (`camera_pod.ino`) in the Arduino IDE.
2. Select the **XIAO ESP32S3** board and the right port.
3. Fill in your details at the top of the sketch — these are the only lines you
   need to change:

   ```cpp
   // WiFi
   const char *ssid     = "YOUR_WIFI_NAME";
   const char *password = "YOUR_WIFI_PASSWORD";

   // Adafruit IO
   const char *aioUsername = "YOUR_USERNAME";
   const char *aioKey      = "YOUR_AIO_KEY";
   const char *aioFeedLatest = "image-latest";        // must match your feed KEY
   const char *aioFeedTime   = "last-photo-time";     // must match your feed KEY
   ```

4. Upload. Open the Serial Monitor at **115200 baud** to watch it connect.

You should see WiFi connect, then repeating lines like:

```
Capturing photo for Adafruit IO...
Upload size: 6020
Adafruit IO response code: 200
```

A **200** means success. Other codes:

- **404** — the feed key in your code doesn't match a feed on Adafruit, or the
  feed doesn't exist. Re-check the keys (Step 1.3).
- **-1** — the connection failed before completing, usually a memory squeeze from
  too large an image. Lower the resolution (see the `AIO_FRAME_SIZE` line in the
  sketch — `FRAMESIZE_HVGA` is a safe choice).

### A note on resolution

Bigger images look sharper but use more memory and can fail to upload. The sketch
uploads at a modest size on purpose. If uploads fail or the board crashes after a
few photos, step the resolution down. `FRAMESIZE_HVGA` (480×320) is a reliable
balance for the XIAO ESP32S3.

---

## Step 3 — Flash the AlgaeScope (spectrometer)

The AlgaeScope is the second board. It measures the algae's colour with an AS7341
spectral sensor and serves its own dashboard on your WiFi — no cloud account
needed for this part, and no separate app to install. The device *is* the app.

Wiring and the parts list (AS7341 sensor, white LED, ring lights, breadboard) are
on the [build page](https://algaerhythm.vercel.app/build.html). Once it's wired:

1. Open the AlgaeScope sketch (`algaescope.ino`) in the Arduino IDE.
2. Fill in your WiFi details at the top — these are the only lines to change:

   ```cpp
   const char* ssid      = "YOUR_WIFI_NAME";
   const char* password  = "YOUR_WIFI_PASSWORD";
   ```

3. Select the **XIAO ESP32S3** board and the right port, then upload.
4. Open the Serial Monitor at **115200 baud**. It connects to WiFi and prints the
   address where the dashboard is reachable.

### Reaching the dashboard

Once it's running, open a browser **on the same WiFi** and go to:

```
http://algaescope.local
```

If your network or device doesn't support `.local` addresses (some Android phones
and locked-down networks don't), use the numeric IP address the Serial Monitor
prints instead — it looks like `http://192.168.1.xx`.

The dashboard gives you live spectral readings, absorbance, the chlorophyll stress
ratio, a growth curve over time, grow-light control, and CSV export. Readings are
held in the device's memory while it's powered on.

> **Note:** reaching the device at `algaescope.local` relies on mDNS being enabled
> in the sketch. If you're using an older copy of the code, use the IP address from
> the Serial Monitor instead.

---

## Step 4 — Add your pod to the shared map

Once your camera is uploading and your feeds are public, you can have your pod
appear on the shared Algaerithm map alongside everyone else's.

**To add your pod, email us at b.soares0520251@arts.ac.uk** with the following:

| Field | Example | Notes |
|-------|---------|-------|
| Guardian name | "Sam R." | A name we can credit on the map. |
| River / area | "River Lea · Hackney" | **Coarse area only** — your town or river reach, never your exact address. |
| Adafruit username | "sam_pods" | From your account menu. |
| Image feed key | "image-latest" | The feed **key**, exactly. |
| Timestamp feed key | "last-photo-time" | The feed **key**, exactly. |
| Both feeds public? | "Yes" | Confirm you've set both to Public (see Step 1) — we can't show your pod otherwise. |

We review each submission (mainly to confirm the feeds are public and the
location is coarse) and then add it to the map. Because we check by hand, there
may be a short wait — thanks for your patience.

> **Privacy:** please give only a coarse location — a river name and town or
> neighbourhood. The map deliberately never shows exact coordinates, so hosting a
> pod never reveals where you live.

---

## How it fits together (for the curious)

```
[ XIAO camera ]  --photo-->  [ Adafruit IO feed (public) ]  <--reads--  [ website ]
```

- The camera uploads a photo to your Adafruit feed every few seconds.
- Your feed is public, so the website can read its latest value from any browser.
- The website holds a list of all registered pods (`pods.json`) and shows a live
  card for each one.

No central server stores your images — the site reads each pod's public feed
directly. That keeps the whole thing free and simple to run.

---

## Common problems

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| Pod doesn't appear on the site | Feed is private | Set feed to Public; confirm with the incognito URL (Step 1). |
| Serial shows `404` | Feed key mismatch | Match the code's feed names to the actual feed **keys** on Adafruit. |
| Serial shows `-1` | Image too large / low memory | Lower `AIO_FRAME_SIZE` to `FRAMESIZE_HVGA`. |
| Image shows on Adafruit but not the site | Page cached, or feed flipped private | Hard-refresh (Ctrl/Cmd+Shift+R); re-check Public setting. |
| Feed quota fills up | Feed History is on | Turn Feed History off, then re-confirm Public. |

---

## License

The code in this repository (the Arduino sketches and website) is released under
the **MIT License** — see the `LICENSE` file. You're free to use, modify, and
build on it; just keep the copyright notice.

The build documentation, designs, and project materials are shared under
**Creative Commons Attribution-ShareAlike (CC BY-SA 4.0)** — use and adapt them
freely, credit Algaerithm, and share your versions under the same terms.

*Algaerithm — MA Biodesign, Central Saint Martins, UAL.*
