import os
from secrets import compare_digest

from fastapi import FastAPI, Header, HTTPException
from pydantic import BaseModel
from typing import Optional

app = FastAPI()
api_token = os.getenv("ADSB_API_TOKEN")
if not api_token:
    raise RuntimeError("ADSB_API_TOKEN must be set before starting the API")

class Aircraft(BaseModel):
    hex: str
    tailnumber: Optional[str]
    longitude: Optional[float]
    latitude: Optional[float]
    altitude: Optional[float]
    velocity: Optional[float]

latest_aircraft: list[Aircraft] = []

@app.post("/aircraft")
async def receive_aircraft(
    aircraft: list[Aircraft],
    authorization: Optional[str] = Header(default=None),
):
    if not compare_digest(authorization or "", f"Bearer {api_token}"):
        raise HTTPException(status_code=401, detail="Invalid or missing bearer token")

    latest_aircraft[:] = aircraft
    print(f"Received aircraft data: {[item.model_dump() for item in aircraft]}")

    return {"status": "success", "count": len(aircraft)}

@app.get("/aircraft")
async def get_aircraft():
    return latest_aircraft