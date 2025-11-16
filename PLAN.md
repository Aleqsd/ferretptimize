# PLAN.md

## Overview  
This project is an ultra fast image compressor written in C with a simple HTML CSS JS frontend. The backend accepts a PNG image through a POST request and produces four compressed outputs: PNG lossless, PNG medium, WebP high quality, and AVIF medium. The backend uses a multithreaded architecture with one main I/O thread and multiple worker threads that use lock free queues for maximum throughput.

## Goals  
- 🏎️ Maximum performance in C  
- 🧵 Worker threads processing compression in parallel  
- 📦 Lock free queues for job dispatch  
- 🖼️ Support PNG, WebP, and AVIF output  
- 🌐 Fast minimal HTTP server written in C  
- 🧩 Pure HTML CSS JS frontend with drag and drop  
- 🧪 Four compression levels produced instantly  
- 📥 Display compressed results and size differences  

## Architecture  
### Backend components  
- 🧵 Main thread  
  - Runs a minimal HTTP server  
  - Accepts uploads  
  - Pushes jobs into a lock free queue  
  - Collects results from an output queue  

- ⚙️ Worker threads  
  - Each thread pops jobs from the input queue  
  - Compresses the input image into four formats  
  - Pushes the results into the output queue  

- 📚 Compression modules  
  - PNG: uses libpng with two compression levels  
  - WebP: uses libwebp for near lossless  
  - AVIF: uses libavif for medium quality  
  - All modules use memory pools and minimal copies  

- 🔗 Job and result data structures  
  - Input job: raw PNG buffer, job id  
  - Output result: four compressed buffers, metadata, job id  

### Frontend components  
- 📄 index.html  
  - Drag and drop zone  
  - Preview of the original image  
  - Four output slots for compressed images  
  - Buttons for downloading each file  

- 🎨 style.css  
  - Clean and modern layout  
  - Large drop zone  
  - Image grid for the four outputs  

- 🧩 app.js  
  - Handles drag and drop  
  - Uses fetch or XHR to upload the PNG  
  - Receives multipart or JSON plus binary blobs  
  - Converts them into image previews  
  - Displays file sizes and download options  

## Backend flow  
- 📥 User drops a PNG on the frontend  
- 🌐 Browser uploads to the C server  
- 🧵 Main thread receives data and enqueues a job  
- ⚙️ Worker threads pop the job and run compression  
- 📦 Four compressed versions are created  
- 📤 Results are enqueued back to the main thread  
- 🌐 Main thread returns a multipart response with the four images  

## Performance features  
- 🚦 Non blocking HTTP server  
- 🧵 Thread pool with N workers (configurable)  
- 🔓 Lock free MPMC queues for job dispatch  
- 🗃️ Memory pools for repeated allocations  
- ♻️ Zero copy reading of uploaded PNG data when possible  
- 🔬 Fine tuned compression parameters  

## File structure  
- 📁 src/server.c  
- 📁 src/worker.c  
- 📁 src/queue.c  
- 📁 src/compress_png.c  
- 📁 src/compress_webp.c  
- 📁 src/compress_avif.c  
- 📁 include/*.h  
- 📁 public/index.html  
- 📁 public/style.css  
- 📁 public/app.js  
- 📁 Makefile  

## Makefile requirements  
- 🧰 Build C server  
- 🧰 Link with libpng, libwebp, libavif, pthread  
- 🧰 Build with -O3 -march=native  

## Expected deliverables for Codex  
- 🧱 Complete backend code for all modules  
- 🧱 Full working HTML CSS JS frontend  
- 🧱 Makefile building the server  
- 🧱 Lock free MPMC queue implementation  
- 🧱 Thread pool setup  
- 🧱 HTTP server capable of binary uploads  
- 🧱 Example usage instructions  
