
# nexlibrOS

> **This is a personal fork of the fantastic [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)** with a handful of what I'd call quality-of-life improvements and a reskin of the homescreen, optimized for the Xteink X4 Pro device with touch screen.
<img width="592" height="944" alt="Screenshot 2026-10-07 at 8 49 47 PM" src="https://github.com/user-attachments/assets/89dd52cd-1d27-4783-91ab-2959411ea57a" />


Specifically I've added

* An understanding and preference for optimized epubs created with the Calibre plugin and served via OPDS (set up and accessible via e.g. https://publiclibrary.of.nhawk.in)
* A simplified, organized settings screen: 
<img width="592" height="944" alt="Screenshot 2026-10-07 at 8 43 23 PM" src="https://github.com/user-attachments/assets/691cea29-2187-4c96-9b66-a0e972fb2b7b" />

* Parsing of additional metadata, including author, title sort fields and tags
* Customizable library organization
<img width="592" height="944" alt="Screenshot 2026-10-07 at 8 46 38 PM" src="https://github.com/user-attachments/assets/225e7b46-d7db-4bc7-ab6c-0bcfaf89be60" />

* Long tap a word in reader mode to define
<img width="592" height="944" alt="Screenshot 2026-10-07 at 8 47 02 PM" src="https://github.com/user-attachments/assets/b001ac4c-2dc0-4142-a438-31d169e8df34" />

* Tab select between different dictionaries
<img width="592" height="944" alt="Screenshot 2026-10-07 at 8 47 26 PM" src="https://github.com/user-attachments/assets/e865ada7-75f3-462e-a42c-4bf21de5b695" />

* Simplified 'Nolan' theme, highlighting recent books with small icon menu for settings, file management, and opening the library
<img width="592" height="944" alt="Screenshot 2026-10-07 at 8 49 47 PM" src="https://github.com/user-attachments/assets/041b7f7b-abe4-41b7-b894-ab04fec374c3" />


Ideally I keep adding features as I think of them, and try to merge upstream where it seems available

This satisfies my immediate wants out of CrossPoint, although I may still explore:
* Highlights (in the long-tap word menu)
* Bookmarks by tapping in upper right hand corner (displayed via dog ear?)
* Gallery mode (very slowly loop between images, e.g. change every 5 min to hour, going to sleep in between)
* Simplification of in-reader menu, not sure how exactly, but its current form does not quite satisfy
* lua plugins or just regular apps of
  * todo
  * calendar
  * unit/currency conversion (really I just want to understand e.g. what 3 rubles in 1850 is worth now when I encounter it in a book)
