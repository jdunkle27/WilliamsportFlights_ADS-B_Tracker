from fastapi import FastAPI
from pydantic import BaseModel
from typing import Optional

app = FastAPI()

class Aircraft(BaseModel):
    hex: str
    tailnumber: Optional[str]
    longitude: Optional[float]
    latitude: Optional[float]
    altitude: Optional[float]
    velocity: Optional[float]

@app.post("/aircraft")
async def receive_aircraft(aircraft: list[Aircraft]):
    print(f"Received aircraft data: {[item.model_dump() for item in aircraft]}")

    return {"status": "success", "count": len(aircraft)}