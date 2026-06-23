from flask import Flask, render_template, jsonify
import serial
import json
import threading
from datetime import datetime

app = Flask(__name__)

latest_data = {
    "A": 0,
    "B": 0,
    "C": 0,
    "D": 0,
    "x": 0,
    "y": 0,
    "threat": "LOW"
}

event_history = []

# CHANGE IF COM PORT CHANGES
ser = serial.Serial("COM4", 115200)

def serial_reader():
    global latest_data
    global event_history

    while True:
        try:
            line = ser.readline().decode(errors='ignore').strip()

            if line.startswith("{"):
                latest_data = json.loads(line)

                if latest_data["threat"] != "LOW":

                    event_history.append({
                        "time": datetime.now().strftime("%H:%M:%S"),
                        "threat": latest_data["threat"],
                        "x": round(latest_data["x"], 2),
                        "y": round(latest_data["y"], 2)
                    })

                    event_history = event_history[-20:]

        except Exception as e:
            print("Error:", e)

threading.Thread(
    target=serial_reader,
    daemon=True
).start()

@app.route("/")
def home():
    return render_template("index.html")

@app.route("/data")
def data():
    return jsonify(latest_data)

@app.route("/history")
def history():
    return jsonify(event_history)

if __name__ == "__main__":
    app.run(
        debug=True,
        use_reloader=False
    )