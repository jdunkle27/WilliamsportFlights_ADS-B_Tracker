import os
import logging
from datetime import datetime, timezone

from fastapi import FastAPI, Header, HTTPException
import mysql.connector
from pydantic import BaseModel
from typing import Optional

app = FastAPI()
logger = logging.getLogger(__name__)
#api_token = os.getenv("ADSB_API_TOKEN")
#if not api_token:
#    raise RuntimeError("ADSB_API_TOKEN must be set before starting the API")

class Aircraft(BaseModel):
    hex: str
    tailnumber: Optional[str]
    longitude: Optional[float]
    latitude: Optional[float]
    altitude: Optional[float]
    rotation: Optional[float]
    velocity: Optional[float]

latest_aircraft: list[Aircraft] = []


def get_mariadb_config() -> dict[str, str | int]:
    return {
        "host": os.getenv("MARIADB_HOST", "127.0.0.1"),
        "port": int(os.getenv("MARIADB_PORT", "3306")),
        "user": os.getenv("MARIADB_USER", "webapp"),
        "password": os.getenv("MARIADB_PASSWORD", ""),
        "database": os.getenv("MARIADB_DATABASE", "williamsport_flights"),
    }


def save_aircraft(aircraft: list[Aircraft]) -> None:
    connection = mysql.connector.connect(**get_mariadb_config())
    cursor = connection.cursor()
    observed_at = datetime.now(timezone.utc).replace(tzinfo=None)

    try:
        for item in aircraft:
            cursor.execute(
                """
                INSERT INTO aircraft (icao_hex, tail_num, last_seen)
                VALUES (%s, %s, %s)
                ON DUPLICATE KEY UPDATE
                    tail_num = COALESCE(VALUES(tail_num), tail_num),
                    last_seen = VALUES(last_seen)
                """,
                (item.hex.upper(), item.tailnumber or None, observed_at),
            )
            cursor.execute(
                "SELECT aircraft_id FROM aircraft WHERE icao_hex = %s",
                (item.hex.upper(),),
            )
            aircraft_id = cursor.fetchone()[0]

            if item.latitude is not None and item.longitude is not None:
                cursor.execute(
                    """
                    INSERT INTO observations (
                        aircraft_id, observation_time, latitude, longitude,
                        altitude, velocity
                    )
                    VALUES (%s, %s, %s, %s, %s, %s)
                    """,
                    (
                        aircraft_id,
                        observed_at,
                        item.latitude,
                        item.longitude,
                        item.altitude,
                        item.velocity,
                    ),
                )

        connection.commit()
    except Exception:
        connection.rollback()
        raise
    finally:
        cursor.close()
        connection.close()


@app.post("/aircraft")
def receive_aircraft(
    aircraft: list[Aircraft],
    authorization: Optional[str] = Header(default=None),
):
#    if not compare_digest(authorization or "", f"Bearer {api_token}"):
#        raise HTTPException(status_code=401, detail="Invalid or missing bearer token")

    try:
        save_aircraft(aircraft)
    except mysql.connector.Error as exc:
        logger.exception("MariaDB write failed")
        raise HTTPException(status_code=503, detail="MariaDB write failed") from exc

    latest_aircraft[:] = aircraft

    return {"status": "success", "count": len(aircraft)}

@app.get("/aircraft")
async def get_aircraft():
    return latest_aircraft