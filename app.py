from fastapi import FastAPI, UploadFile, File, Header, HTTPException
import httpx

# === НАСТРОЙКИ ===
COMPREFACE_BASE = "http://localhost:8000"  # CompreFace работает на этом ПК
COMPREFACE_API_KEY = "******"

# Порог: чем выше, тем строже. Обычно 0.6-0.75 хорошая отправная точка.
THRESHOLD = 0.8

# Простой "секрет" чтобы случайные устройства в Wi-Fi не слали вам фотки.
DEVICE_SHARED_KEY = "*******"
# ==================

app = FastAPI()


@app.get("/health")
async def health():
    return {"ok": True}


@app.post("/check")
async def check(
    file: UploadFile = File(...),
    x_device_key: str | None = Header(default=None),
):
    # 1) Авторизация устройства (опционально, но рекомендую)
    if x_device_key != DEVICE_SHARED_KEY:
        raise HTTPException(status_code=401, detail="Bad device key")

    img = await file.read()
    if not img:
        raise HTTPException(status_code=400, detail="Empty file")

    # 2) Отправка в CompreFace recognize
    files = {"file": (file.filename or "frame.jpg", img, file.content_type or "image/jpeg")}
    headers = {"x-api-key": COMPREFACE_API_KEY}
    params = {
        "limit": 1,
        "prediction_count": 1,
    }

    async with httpx.AsyncClient(timeout=30) as client:
        r = await client.post(
            f"{COMPREFACE_BASE}/api/v1/recognition/recognize",
            params=params,
            headers=headers,
            files=files,
        )

    # Если API key неверный или сервис не запущен — будет 401/5xx
    if r.status_code != 200:
        raise HTTPException(status_code=502, detail=f"CompreFace error {r.status_code}: {r.text}")

    data = r.json()

    # 3) Упрощаем ответ
    known = False
    subject = None
    similarity = None

    results = data.get("result") or []
    if results:
        subs = results[0].get("subjects") or []
        if subs:
            subject = subs[0].get("subject")
            similarity = subs[0].get("similarity")
            if similarity is not None and similarity >= THRESHOLD:
                known = True

    return {"known": known, "subject": subject, "similarity": similarity}
