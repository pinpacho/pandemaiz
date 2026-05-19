FROM python:3.11-slim

WORKDIR /app

# Instalar dependencias del sistema requeridas por ObsPy
RUN apt-get update && apt-get install -y --no-install-recommends \
    libxml2 libxslt1.1 \
    && rm -rf /var/lib/apt/lists/*

COPY requirements-backend.txt .
RUN pip install --no-cache-dir -r requirements-backend.txt

COPY . .

EXPOSE 8000

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
