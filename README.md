# World of Warcraft 3.3.5a Raw Input Mouse Fix

The code was AI generated under strict guidance by myself.  

I'm not a fan of vibe coding, but I do believe AI is a great tool to accelerate coding for people with some actual dev background.

You can also see a huge improvement in ground clutter detail by typing /console groundeffectdist 512. If it runs too slow, try /console groundeffectdensity 64 or lower. This isn't a simple distance extension, it is an optimized rendering algorithm for keeping performance high while you can still see new ground effects being rendered much further away.

The use of this on some private servers might get you banned if they have certain anti-cheat measures in place.  

It's not a cheat in the slightest, but whether it's a cheat or legit, the same methods are used to modify the game - and that's what gets you flagged.

It is possible for somebody extremely skilled to pull the mouse fix machine code from the binary, and plop it into a code cave somewhere in the wow binary.  In fact, I've done it but can't release it for copyright reasons.  Maybe some of the stricter and better funded private servers will at least apply that for their players.

# About this project:

A DLL-based mouse-input improvement for the **World of Warcraft 3.3.5a client** that introduces Windows **Raw Input** into the game's existing mouselook system.

The goal is not to replace WoW's mouse handling or rewrite its camera code. Instead, this project captures relative mouse movement independently of WoW's cursor-position polling and feeds that movement back through the input path WoW already understands.

This makes mouselook substantially more reliable during fast mouse movement, rapid turns, high polling rates, and other situations where the original cursor-based input model can lose movement.

---

## Why Raw Input?

The original 3.3.5a client uses the Windows cursor position as part of its mouselook system.

Conceptually, the process looks something like this:

```text
Physical mouse movement
        │
        ▼
Windows cursor position
        │
        ▼
WoW reads cursor position
        │
        ▼
Calculate movement relative to anchor
        │
        ▼
Rotate camera
        │
        ▼
WoW recenters cursor
```

This works reasonably well under normal conditions, but it has an inherent weakness.

WoW is sampling an **absolute cursor position** to determine how much the mouse moved.

That means physical mouse movement has to make it through the Windows cursor-position mechanism before WoW samples it.

During mouselook, WoW also repeatedly moves the cursor back toward an anchor position.

Under sufficiently fast movement, the timing can become problematic:

```text
Mouse moves
    │
    ▼
WoW reads cursor
    │
    ▼
WoW calculates movement
    │
    ▼
WoW recenters cursor
    │
    ├──── mouse moves again
    │
    ▼
Next cursor read
```

Some of the physical movement can occur between those operations without being represented in the cursor position WoW eventually reads.

The result can be lost or inconsistent camera movement.

---

# What Does the Problem Actually Feel Like?

The underlying problem is often difficult to identify because the game does not simply stop responding to the mouse.

Instead, users may experience symptoms such as:

* Slow mouse movement works normally.
* Fast mouse swipes rotate the camera less than expected.
* Rapid 180° or 360° turns can lose part of the movement.
* Very fast flicks can produce inconsistent camera rotation.
* Higher mouse DPI or polling rates can make the problem more noticeable.
* Physically moving the mouse a certain distance does not always produce the same camera movement.
* Mouse sensitivity can change how noticeable the problem is.
* Some apparent mouse problems may change depending on whether the game is windowed or fullscreen.

These symptoms can make the problem look like a sensitivity, DPI, Windows scaling, or coordinate-conversion issue.

Some of those issues are real.

However, they are not all the same problem.

---

# What Raw Input Changes

Windows Raw Input provides **relative mouse movement** directly from the input device.

Instead of asking:

> "Where is the cursor right now?"

Raw Input effectively provides:

> "How far did the mouse move?"

For example:

```text
Mouse movement:
    X = +14
    Y = -3
```

This information is independent of the current screen cursor position.

It also does not depend on WoW successfully observing the cursor before it recenters it.

The project therefore uses Raw Input as a **relative-motion side channel**:

```text
                 ┌──────────────────────────┐
                 │      Physical Mouse      │
                 └────────────┬─────────────┘
                              │
                    Windows Raw Input
                              │
                              ▼
                    Relative X/Y movement
                              │
                              ▼
                       Accumulator
                              │
                              ▼
                WoW's existing cursor read
                              │
                              ▼
                       Existing delta
                              │
                              ▼
                         Mouselook
```

WoW does not need to understand Raw Input.

It continues using its existing mouselook code.

---

# This Project Does Not Replace WoW's Mouse System

An important design goal is to preserve as much of the original client behavior as possible.

This project does **not** replace WoW's camera calculations.

It does not implement a new mouselook system.

It does not directly rotate the camera.

It does not require rewriting WoW's mouse-event processing.

Instead, it changes the source of the position information that WoW uses when determining mouse movement.

The existing logic remains approximately:

```text
cursor position
      -
anchor position
      =
mouse delta
```

The project simply makes the cursor position returned to that logic represent:

```text
anchor position + accumulated Raw Input movement
```

Therefore:

```text
(anchor + rawDelta) - anchor = rawDelta
```

The existing WoW mouselook code effectively receives the same type of delta it expected before, but the delta is no longer dependent on the Windows cursor keeping up with physical mouse movement.

---

# Core Technique

The client contains an internal resolver that obtains the physical cursor position.

For the targeted 3.3.5a executable, the relevant function is:

```text
0x00868C10
```

WoW caches the function pointer used by this resolver at:

```text
0x00D41590
```

Normally, that cached pointer refers to the Windows cursor-position implementation.

This project replaces the cached resolver pointer with its own function.

No modification to the resolver's executable code is required.

Conceptually:

```text
WoW
 │
 │ calls its existing cursor resolver
 ▼
0x00868C10
 │
 │ uses cached function pointer
 ▼
0x00D41590
 │
 ├── normally → Windows cursor API
 │
 └── this project → custom resolver
```

This allows the existing WoW input path to remain intact.

---

# Synthetic Cursor Position

The custom resolver does not simply return the current Windows cursor position.

While mouselook is active, it constructs a synthetic position:

```text
synthetic X = WoW anchor X + accumulated Raw Input X
synthetic Y = WoW anchor Y + accumulated Raw Input Y
```

The relevant WoW anchor values are:

```text
X: 0x00D413EC
Y: 0x00D413F0
```

The accumulated Raw Input movement is then consumed when WoW asks for the cursor position.

From WoW's perspective, it still sees a normal cursor position.

For example:

```text
WoW anchor:
    X = 500
    Y = 400

Raw Input:
    X = +37
    Y = -12

Returned cursor:
    X = 537
    Y = 388
```

WoW then performs its existing calculation:

```text
537 - 500 = +37
388 - 400 = -12
```

The result is exactly the relative mouse movement reported by Raw Input.

---

# Why This Avoids Movement Loss

The important difference is **when the movement is captured**.

The original path depends on this:

```text
mouse movement
      ↓
cursor movement
      ↓
WoW reads cursor
      ↓
WoW recenters cursor
```

The Raw Input path records the physical movement independently:

```text
mouse movement
      ↓
Raw Input
      ↓
accumulator
      ↓
WoW reads synthetic cursor
      ↓
existing mouselook calculation
```

Cursor recentering therefore does not destroy the movement information.

Even if WoW moves the Windows cursor back to its anchor immediately afterward, the relative physical movement has already been recorded.

---

# Why Not Simply Hook Mouse Messages?

A tempting implementation would be to intercept `WM_MOUSEMOVE` and convert those messages into relative movement.

That does not solve the underlying problem as cleanly.

`WM_MOUSEMOVE` is still based on cursor movement.

It is therefore downstream of the same cursor-position/recentering mechanism that causes the original problem.

Raw Input is different.

It reports relative device movement independently of the screen cursor.

That makes it much better suited to correcting a cursor-based mouselook implementation.

---

# Limitations of Existing Fixes

There is already a known fix for a different class of mouse problems in the 3.3.5a client.

The commonly used approach inserts a call to:

```cpp
ScreenToClient()
```

into the client's mouse-input path.

The basic idea is:

```text
Windows screen coordinates
          │
          ▼
    ScreenToClient()
          │
          ▼
WoW client/window coordinates
```

This is a legitimate and useful fix.

However, it addresses a **coordinate-space problem**, not the fundamental movement-capture problem described here.

The two issues can produce superficially similar symptoms.

### Coordinate-space problem

WoW receives a cursor position, but interprets coordinates from the wrong coordinate system.

This can cause:

* Cursor offsets.
* Incorrect movement when the window is moved.
* Different behavior between windowed and fullscreen modes.
* Mouse-position inconsistencies depending on window placement.
* Incorrect client-relative coordinates.

`ScreenToClient()` can correct these problems.

### Movement-capture problem

The coordinates themselves may be correct, but the cursor does not necessarily represent every physical mouse movement that occurred between WoW's reads and cursor recentering.

This can cause:

* Fast swipes producing less camera movement than expected.
* Rapid 180°/360° turns losing movement.
* Intermittent skips during fast movement.
* Slow movement working while fast movement does not.
* High DPI or high polling rates making the problem more apparent.
* Physical mouse travel not consistently corresponding to camera rotation.

`ScreenToClient()` does not turn cursor-based mouselook into relative mouse input.

It only makes the cursor coordinates correct for the coordinate space being used.

### Why the distinction matters

It is possible for a client to have both problems.

For example:

```text
                 Mouse problem
                      │
             ┌────────┴────────┐
             │                 │
      Wrong coordinate     Lost movement
          space             during sampling
             │                 │
             ▼                 ▼
      ScreenToClient()      Raw Input
```

This is why installing the known coordinate conversion fix can improve some symptoms while leaving fast-turn movement loss unresolved.

A useful diagnostic rule is:

| Symptom                                                 | Likely problem                   |
| ------------------------------------------------------- | -------------------------------- |
| Moving the game window changes mouse behavior           | Coordinate-space issue           |
| Cursor/camera offset                                    | Coordinate-space issue           |
| Windowed vs. fullscreen changes behavior                | Potential coordinate-space issue |
| Fast swipes lose camera rotation                        | Movement-capture issue           |
| Rapid turns skip movement                               | Movement-capture issue           |
| Slow movement works, fast movement does not             | Movement-capture issue           |
| High DPI/polling exposes the problem                    | Movement-capture issue           |
| Physical mouse travel exceeds resulting camera rotation | Movement-capture issue           |

In short:

> The known `ScreenToClient()` fix corrects **where** the cursor is measured. This project corrects **how physical movement is captured**.

---

# Why Preserve WoW's Existing Mouselook?

Implementing a completely independent camera-input system would create unnecessary compatibility problems.

WoW already has working logic for:

* Mouselook activation.
* Mouse sensitivity.
* Camera rotation.
* Camera state.
* Cursor recentering.
* UI interaction.
* Input state transitions.

Replacing all of that would require reproducing behavior that the client already handles correctly.

The Raw Input approach only changes the input data entering that system.

This minimizes the amount of client behavior that has to be reimplemented.

---

# Mouselook State Handling

Raw Input can be received even when the game is not actively processing mouselook.

The project therefore checks WoW's existing mouselook state.

For the targeted client:

```text
0x00D4156C
```

is used as the mouselook state.

The relevant state is:

```text
1 = mouselook active
```

When mouselook is inactive, accumulated Raw Input movement is discarded.

This is important because otherwise movement performed while the game is not looking could remain buffered and unexpectedly affect the camera when mouselook begins.

Conceptually:

```text
Raw Input
   │
   ▼
Mouselook active?
   │
   ├── No  → discard
   │
   └── Yes → accumulate
```

---

# Raw Input Registration

The DLL creates a hidden message-only window and registers for Windows Raw Input using the standard mouse HID usage:

```text
Usage Page: 0x01  Generic Desktop
Usage:      0x02  Mouse
```

The registration uses:

```text
RIDEV_INPUTSINK
```

so that the hidden window can receive Raw Input independently of the normal WoW window message path.

Only relative mouse movement is used.

Absolute-position reports are ignored.

---

# Focus Handling

Because `RIDEV_INPUTSINK` can receive input while the target application is not the foreground process, the implementation explicitly checks whether WoW is currently foreground.

If the game is not active, accumulated movement is cleared.

This prevents stale movement from being applied after returning to the game.

Conceptually:

```text
Raw Input
    │
    ▼
Is WoW foreground?
    │
    ├── No  → clear accumulator
    │
    └── Yes → process movement
```

This also prevents background mouse activity from unexpectedly affecting the next mouselook operation.

---

# Fractional Scaling

The project supports configurable Raw Input scaling through:

```text
WOW_MOUSE_SCALE
```

The default scale is:

```text
1.0
```

Values are constrained to a reasonable range.

Scaling is performed using fractional accumulation rather than simply truncating every individual movement report.

For example, if the effective movement is:

```text
0.4 + 0.4 + 0.4
```

a naive integer conversion could lose the movement.

Instead, the fractional remainder is retained until enough movement exists to produce another integer unit.

This avoids systematic movement loss at low scaling factors.

---

# Thread Safety

Raw Input is accumulated from the Raw Input processing thread while WoW consumes the movement from its cursor resolver.

The implementation uses atomic operations for the accumulated X/Y values.

The resolver effectively performs:

```text
deltaX = accumulatedX
deltaY = accumulatedY
```

while resetting the consumed values.

This prevents the two sides from requiring a traditional mutex around every mouse movement.

The design is intentionally lightweight because mouse input can occur at very high polling rates.

---

# Relevant Client Addresses

This project targets a specific **World of Warcraft 3.3.5a executable**.

The current implementation uses the following addresses:

| Address      | Purpose                          |
| ------------ | -------------------------------- |
| `0x00D413EC` | WoW X cursor anchor              |
| `0x00D413F0` | WoW Y cursor anchor              |
| `0x00D4156C` | Mouselook state                  |
| `0x00D41590` | Cached cursor-position resolver  |
| `0x00868C10` | Cursor-position resolver         |
| `0x0086A020` | Related mouselook state handling |

These addresses are specific to the targeted executable.

They should **not** be assumed to apply to other WoW builds.

---

# Safety and Version Checking

Because the implementation relies on fixed addresses, blindly installing it into another executable would be unsafe.

The DLL therefore verifies the expected executable characteristics and checks the resolver's expected machine-code signature before replacing the cached function pointer.

For example, the resolver is expected to begin with a signature equivalent to:

```text
55 8B EC A1 90 15 D4 00
```

The resolver cache is also checked before modification.

If the expected structure is not present, the mouse modification is not installed.

This is intended to fail safely rather than corrupting an unexpected client version.

---

# Why Use the Resolver Cache Instead of a Code Detour?

There are several ways to modify WoW's input path.

One option would be to overwrite instructions inside the resolver with a JMP to custom code.

This project instead takes advantage of the fact that WoW already uses an indirect function pointer.

Conceptually:

```text
WoW resolver
     │
     ▼
function pointer
     │
     ▼
GetPhysicalCursorPos()
```

The project changes only the pointer:

```text
WoW resolver
     │
     ▼
function pointer
     │
     ▼
custom resolver
```

Advantages include:

* No inline detour required.
* No trampoline required.
* Less executable code modification.
* The original resolver code remains intact.
* Easy to validate before installation.
* Easier to remove or disable.
* Lower risk of overwriting adjacent instructions.

The tradeoff is that this technique depends on the exact internal structure of the targeted executable.

---

# Interaction With `ScreenToClient()`

The Raw Input technique and `ScreenToClient()` technique solve different problems.

They should therefore not automatically be combined.

The known `ScreenToClient()` patch effectively does:

```text
screen coordinates
       ↓
ScreenToClient()
       ↓
client coordinates
```

The Raw Input resolver instead generates:

```text
WoW anchor
     +
Raw Input delta
     ↓
synthetic position
```

If the synthetic position is already expressed in the coordinate system expected by the downstream WoW code, applying an additional coordinate transformation could produce incorrect results.

Therefore, any combination of the two approaches should first establish exactly which coordinate space the resolver and its callers expect.

The important distinction is:

```text
ScreenToClient()
    = coordinate correction

Raw Input resolver
    = movement-capture correction
```

They are complementary in theory, but they are not interchangeable and should not be stacked blindly.

---

# Comparison

| Approach               | Corrects coordinates | Captures relative movement | Avoids cursor sampling loss | Preserves WoW mouselook |
| ---------------------- | -------------------: | -------------------------: | --------------------------: | ----------------------: |
| Original WoW input     |                    ❌ |                          ❌ |                           ❌ |                       ✅ |
| `ScreenToClient()` fix |                    ✅ |                          ❌ |                           ❌ |                       ✅ |
| `WM_MOUSEMOVE` hook    |                Maybe |                          ❌ |                           ❌ |                  Mostly |
| Direct camera rewrite  |              Depends |                          ✅ |                           ✅ |                       ❌ |
| Raw Input resolver     |              Depends |                          ✅ |                           ✅ |                       ✅ |

The Raw Input resolver is specifically designed to preserve the original client behavior while eliminating the weakness caused by using the Windows cursor as the authoritative representation of physical mouse movement.

---

# Design Goals

The project is intentionally designed around a few principles:

### 1. Preserve existing WoW behavior

Do not replace camera logic that already works.

### 2. Capture physical movement independently

Use Raw Input so movement does not depend on cursor recentering.

### 3. Minimize client modification

Prefer existing function-pointer infrastructure over unnecessary code patches.

### 4. Fail safely

Verify the target executable and expected internal structures before installing the modification.

### 5. Avoid stale input

Discard movement when mouselook is inactive or WoW is not foreground.

### 6. Keep the implementation lightweight

Mouse input can arrive at hundreds or thousands of reports per second. The input path should therefore avoid unnecessary synchronization overhead.

---

# Technical Summary

The complete input flow is approximately:

```text
                    Physical Mouse
                          │
                          ▼
                   Windows Raw Input
                          │
                          ▼
                  Hidden Input Window
                          │
                          ▼
                 Relative X/Y movement
                          │
                          ▼
                    Accumulator
                          │
                          │
                          ▼
WoW mouselook ──► cursor resolver
                          │
                          ▼
                 Custom resolver
                          │
                          ▼
              WoW anchor + Raw delta
                          │
                          ▼
                 Existing WoW code
                          │
                          ▼
             Cursor - WoW anchor
                          │
                          ▼
                  Existing delta
                          │
                          ▼
                    Camera rotation
```

The key property is that WoW continues to believe it is reading a cursor position.

It simply receives a cursor position whose displacement from the anchor represents actual relative mouse movement captured through Raw Input.

---

# What This Actually Fixes

This project is primarily intended to address movement loss caused by WoW's cursor-based mouselook implementation.

It is particularly useful when the following behavior is observed:

* Fast mouse movements do not rotate the camera proportionally.
* Rapid turns lose movement.
* High mouse polling rates expose inconsistent camera movement.
* Very fast flicks behave differently from slow movements.
* Increasing mouse sensitivity does not actually eliminate the problem.
* Physical mouse movement and camera rotation become inconsistent during fast motion.

It is **not** intended to be a universal solution for every possible WoW mouse problem.

If the problem is caused by incorrect screen/client coordinate conversion, `ScreenToClient()` or another coordinate-space correction may still be necessary.

---

# Limitations

This implementation is tied to the internal structure of a particular WoW 3.3.5a executable.

It therefore has several inherent limitations:

* Fixed addresses are version-specific.
* Changes to the executable can invalidate the addresses.
* Different client builds may use different resolver implementations.
* Multiple physical mouse/HID devices may contribute Raw Input unless additional device filtering is implemented.
* Windows Raw Input behavior still depends on the operating system and device driver stack.
* This project does not modify WoW's underlying camera mathematics.
* It does not automatically correct unrelated coordinate-space problems.
* Extremely unusual input devices or nonstandard HID configurations may require additional handling.

The project intentionally prioritizes a small, targeted modification over attempting to create a universal replacement for WoW's input system.

---

# Conclusion

World of Warcraft 3.3.5a's original mouselook system was designed around an absolute Windows cursor that is repeatedly read and recentered.

That approach works, but it means physical mouse movement is indirectly represented through cursor position.

The fundamental improvement in this project is to introduce **Raw Input as the authoritative source of relative physical movement**, while allowing WoW to continue using its existing cursor-based mouselook implementation.

The result is:

```text
Raw Input
   ↓
relative movement
   ↓
synthetic cursor position
   ↓
existing WoW input code
   ↓
existing mouselook
```

Rather than replacing WoW's mouse system, the project supplies it with better input data.

This distinction is important.

The goal is not to make WoW understand Raw Input.

The goal is to make WoW's existing mouselook code receive the movement it was supposed to receive in the first place.

# Bonus Rendering Features

In addition to the Raw Input mouse improvements, this DLL also extends two of the 3.3.5a client's rendering-distance controls.

## Increased Far Clip Distance

The maximum value permitted by the `farclip` CVar has been **doubled**.

No additional rendering optimizations were implemented for `farclip`. Increasing it simply allows the client to render the world farther than the original client permitted, so the normal performance cost associated with rendering additional distant geometry still applies.

---

# Extended Ground Effect Distance

The more significant rendering enhancement is the extension of `groundEffectDist`.

`groundEffectDist` controls how far from the player the client generates and renders ground effects such as grass and other small environmental details.

The original client places a relatively low ceiling on this distance. Simply raising that limit, however, creates a major performance problem.

## Why Ground Effects Are Expensive

Ground effects are not just a matter of telling the GPU to draw more grass.

The client has to determine **where ground effects should exist** across the terrain surrounding the player.

As the effect distance increases, the area that needs to be considered grows rapidly.

A naive implementation of a much larger distance would therefore look roughly like this:

```text
Small ground-effect distance
        │
        ▼
Small area to process
        │
        ▼
Low CPU cost


Large ground-effect distance
        │
        ▼
Much larger area to process
        │
        ▼
Much higher CPU cost
```

Simply increasing the CVar limit without changing the underlying processing can therefore make distant grass dramatically more expensive than it appears from the relatively small increase in visible detail.

This is especially noticeable when the player is moving through large outdoor areas where large amounts of terrain must continually be evaluated.

---

## The Problem With Extending the Original Algorithm

The original ground-effect generation logic was designed around the relatively short distances supported by the original client.

When that distance is extended substantially, the original algorithm ends up doing work over a much larger region.

The important observation is that **the player does not need every part of that larger region to be processed with the same granularity and frequency**.

The additional distance is primarily useful for providing a visual transition into the distance.

Treating every additional portion of terrain exactly like the nearby terrain wastes processing effort.

This project therefore does more than simply raise the CVar limit.

It modifies the ground-effect distance processing so that the extended range can be handled substantially more efficiently.

---

## What the Updated Algorithm Changes

The modified implementation separates the concept of:

1. **How far ground effects are allowed to exist**, and
2. **How much work is required to maintain those effects at that distance.**

The result is that increasing `groundEffectDist` no longer requires the original processing model to scale directly with the entire expanded area.

Instead, the additional distance is handled in a way that significantly reduces the amount of CPU work required compared with simply extending the original algorithm unchanged.

Conceptually:

```text
Original client
────────────────────────────────────

          Player
             ●
        ┌─────────┐
        │ Process │
        │ terrain │
        │ at full │
        │ range   │
        └─────────┘

Increasing distance
             ↓

        ┌─────────────────────┐
        │ Process substantially│
        │ more terrain using  │
        │ the same approach   │
        └─────────────────────┘

             ↓

       Large CPU increase
```

The updated implementation instead makes the extended region considerably cheaper to process:

```text
Updated implementation
────────────────────────────────────

              Player
                 ●
          ┌────────────┐
          │ Near area  │
          │ detailed   │
          └────────────┘
                 │
          ┌───────────────┐
          │ Extended area │
          │ optimized     │
          │ processing    │
          └───────────────┘

                 ↓

      Much larger visible range
                 +
      substantially less overhead
```

The important point is that the optimization is applied to the **ground-effect generation/processing itself**, rather than merely hiding the performance cost behind a higher CVar limit.

---

## Why This Makes a Large Difference

The visual benefit of extending ground-effect distance is relatively straightforward:

```text
Original:

Player ───────────────► grass disappears


Extended:

Player ─────────────────────────────────►
                         grass remains visible
```

The performance problem comes from making the client maintain all of that additional area using the original algorithm.

The modified implementation reduces the amount of work associated with the extended portion of the range.

That means the user can push `groundEffectDist` considerably farther before the CPU cost becomes prohibitive.

This is particularly valuable on modern systems, where the original client can leave substantial CPU performance unused while still imposing its legacy rendering limits.

---

## What This Does *Not* Mean

This optimization does **not** make distant ground effects free.

There is still an unavoidable cost associated with:

* Generating additional ground effects.
* Maintaining additional terrain detail.
* Sending additional geometry to the renderer.
* Rendering more objects.
* Higher scene complexity in dense areas.

The purpose of the modification is instead to avoid the **disproportionately large CPU penalty of the original ground-effect processing algorithm** when the distance is extended.

In other words:

> More visible ground detail still costs performance. The difference is that the client no longer has to pay the full cost of processing the expanded distance using its original, poorly scaling approach.

---

## The Practical Result

With the original client, there is a fairly hard choice:

```text
Low groundEffectDist
    ↓
Good performance
    ↓
Grass/detail disappears relatively close to the player
```

or:

```text
Higher groundEffectDist
    ↓
More visible detail
    ↓
Rapidly increasing CPU workload
```

This project changes that tradeoff:

```text
Higher groundEffectDist
    ↓
More distant ground detail
    ↓
Optimized extended-range processing
    ↓
Much smaller performance penalty
```

The exact benefit varies by area and hardware, but the goal is to make substantially higher ground-effect distances practical rather than merely making the CVar accept a larger number.

---

## Summary

| Feature                  | Change                                                        |
| ------------------------ | ------------------------------------------------------------- |
| `farclip`                | Maximum value doubled                                         |
| `farclip` optimization   | None; additional distance carries the normal rendering cost   |
| `groundEffectDist`       | Maximum distance substantially extended                       |
| Ground-effect processing | Reworked to reduce the CPU cost of the extended range         |
| Goal                     | Make long-distance ground detail practical on modern hardware |

The important enhancement is therefore not simply:

> **"The client now accepts a larger `groundEffectDist`."**

It is:

> **"The client can process a much larger ground-effect distance without scaling the original CPU workload in the same way."**

That distinction is what makes the extended ground-effect range useful rather than merely experimental.

